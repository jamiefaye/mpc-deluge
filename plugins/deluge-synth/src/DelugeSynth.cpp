// Deluge Synth - an MPC instrument plugin by Fentonia
//
// The Synthstrom Deluge's synth engine, running the Deluge community firmware's own code: this
// file is only the plugin shell. It picks a SYNTH preset file, hands MIDI notes to the engine and
// copies the engine's output to the MPC. Everything that makes sound is in the engine
// (hosted/engine.h, built from the firmware into libdeluge.a).
//
// Limits of this first version:
//   * The real engine is in the builds for 32-bit and 64-bit ARM MPCs and for a 64-bit ARM
//     computer. An Intel build contains a placeholder tone (engine_stub.cpp).
//   * One instance per project makes sound. The firmware keeps its state in globals, so a second
//     instance in the same MPC process stays silent.
//   * Presets come from Content/SYNTHS/ inside the plugin folder, and their samples from
//     Content/SAMPLES/ (the folder layout of a Deluge SD card). Samples are loaded whole into memory.
//   * No tempo sync yet (synced LFOs, arpeggiator and delay have no clock), and no MPE.
//
// Gold knobs: parameters Gold 1..16 are the sixteen gold-knob assignments stored in the preset
// (eight buttons times two knobs on a Deluge). Their value text names what each controls.
//
// The shortcut grid: one parameter for every pad of the Deluge's 16 x 8 grid that edits a value of
// the sound (grid_table.h, generated from the firmware; hosted/matrix.h reads and writes them
// through the firmware's own menu items). A pad that does nothing in the loaded preset (an FM
// control in a subtractive sound) shows "-". Gold knobs and pads can reach the same thing, and one
// pad can change what others do, so after any change the audio thread reads everything back and
// the worker thread makes the parameters show it (see "engine parameters" below).
//
// Choosing a preset: the Preset parameter is the position in the sorted list of preset files, and
// Folder is the folder it sits in. Four button parameters step through presets and folders.
//
// Instances: the process has one engine (the firmware keeps its state in globals), and every
// plugin instance has an instance of its own inside it (engine.h): its own song, so its own
// reverb and master stage, sharing the voice pools, the sample files and the memory. Past
// kMaxInstances (16) a plugin instance gets none and is silent.
//
// Threads: the engine must never be entered by two threads at once, whichever instances they
// belong to. Everything but process() takes the engine lock with EngineHold, which also makes
// the instance current. process() (an audio thread) never waits for those: while one holds or
// wants the lock it outputs silence, so a preset loading in one instance is a gap in all of
// them (the load cannot run beside the render, the firmware's allocator being one of the shared
// things). It does wait for another instance's process(), which is over in a fraction of a
// block. A worker thread owned by the plugin loads presets.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_plugin.h"

#include "engine.h"
#include "grid_table.h"
#include "matrix.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

enum ParamIndex
{
    kVolume = 0,     // dB. First on purpose: the MPC nudges parameter 0 on every load.
    kPreset,         // index into the preset files found at start-up
    kFolder,         // index of the folder the preset is in; setting it jumps to that folder's first preset
    kFolderPrev,     // momentary buttons: the plugin acts on a press and puts the button back
    kFolderNext,
    kPresetPrev,
    kPresetNext,
    kGold,           // first of 16: the preset's gold-knob assignments, in the Deluge's order
    kGrid = kGold + deluge_hosted::kNumKnobs,   // first of the shortcut-grid pads, in grid_table.h's order
    kFine = kGrid + grid::kNumCells,            // the cents halves of the transposes (see isSplit), in grid order
    kNumParams = kFine + 5
};

// "Engine parameters": the parameters that mirror a value held by the engine, which are the gold
// knobs and then the grid pads. Engine parameter m is plugin parameter kGold + m.
constexpr int kNumEngineParams = deluge_hosted::kNumKnobs + grid::kNumCells;
constexpr int kFirstCell = deluge_hosted::kNumKnobs;   // engine parameter of grid::kCells[0]

// A transpose is one number in the firmware, in cents, -9600..9600. An MPC moves a continuous
// parameter a hundredth of its range per click of a Q-Link or the data wheel, which here is 1.92
// semitones, and a thousandth with Shift; it only steps exactly through a parameter with named
// choices. So each transpose is two such parameters: semitones (the pad's own id) and cents
// (-50..49, the kFine parameters). Together they are the one engine value.
constexpr int kSemitones = 96;
constexpr bool isSplit(const grid::Cell& cell)
{
    return cell.kind == grid::Kind::Number && cell.text == grid::Text::Hundredths && cell.max == kSemitones * 100;
}
constexpr int semitonesOf(int cents) { return (cents + 50 + kSemitones * 100) / 100 - kSemitones; }   // rounds to nearest
constexpr int centsOf(int cents) { return cents - 100 * semitonesOf(cents); }                          // -50..49
constexpr int slotOf(const grid::Cell& cell) { return deluge_hosted::gridSlot(cell.layer, cell.x, cell.y); }

// One engine per process (see the header comment).
std::mutex gEngineLock;
std::atomic<int> gEngineHolds{0};   // threads holding or waiting for the lock that are not in process()

// The engine, for everything but process(): the lock, and this plugin instance's engine instance
// made current.
struct EngineHold
{
    explicit EngineHold(int instance)
    {
        gEngineHolds.fetch_add(1);
        gEngineLock.lock();
        if (instance >= 0) deluge_hosted::engineUse(instance);
    }
    ~EngineHold()
    {
        gEngineLock.unlock();
        gEngineHolds.fetch_sub(1);
    }
    EngineHold(const EngineHold&) = delete;
    EngineHold& operator=(const EngineHold&) = delete;
};

// The engine for process(). False (silence) while a preset load or the like has it or is waiting;
// another instance's process() is waited for, at most about two blocks' worth, on the assumption
// that an MPC renders its tracks on more than one thread. (If it does not, this never waits.)
inline bool engineForAudio(int instance)
{
    if (instance < 0) return false;
    const auto start = std::chrono::steady_clock::now();
    for (int spins = 0;; ++spins)
    {
        if (gEngineHolds.load(std::memory_order_acquire) > 0) return false;
        if (gEngineLock.try_lock())
        {
            deluge_hosted::engineUse(instance);
            return true;
        }
        if ((spins & 255) == 255 && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(6)) return false;
    }
}

constexpr int kMaxPresets = 4096;

} // namespace

class DelugeSynth : public mpc::Plugin
{
public:
    DelugeSynth() : mpc::Plugin(makeInfo())
    {
        scanPresets();
        addParam(mpc::floatParam("volume", "Volume", -40.0f, 12.0f, 0.0f, "dB", 1));
        // One step per preset or folder found, so a knob or the data wheel cannot skip any. (A
        // project stores the preset's path, so these ranges may differ from one start to the next.)
        addParam(mpc::intParam("preset", "Preset", 0, std::max(0, (int)presets_.size() - 1), 0));
        addParam(mpc::intParam("folder", "Folder", 0, std::max(0, (int)folders_.size() - 1), 0));
        // The MPC has no list widget, so stepping is done with buttons. Each is a parameter that
        // is acted on when it goes on (parameterChanged) and that the worker thread puts back to
        // off a moment later.
        addParam(mpc::boolParam("folderprev", "Folder -", false));
        addParam(mpc::boolParam("foldernext", "Folder +", false));
        addParam(mpc::boolParam("presetprev", "Preset -", false));
        addParam(mpc::boolParam("presetnext", "Preset +", false));
        for (auto& since : buttonOnSince_) since.store(0);
        // The Deluge's gold knobs: what each one controls is part of the preset, so the names here
        // are only positions; the value text carries the preset's own name for it.
        for (int k = 0; k < deluge_hosted::kNumKnobs; ++k)
        {
            char id[16], name[16];
            std::snprintf(id, sizeof id, "gold%d", k + 1);
            std::snprintf(name, sizeof name, "Gold %d", k + 1);
            addParam(mpc::intParam(id, name, deluge_hosted::kKnobMin, deluge_hosted::kKnobMax, 0));
            knobNames_[k][0] = 0;
            snapKnobValid_[k].store(false);
        }
        // The shortcut grid. Ids, and the order here, are grid_table.h's and must not change.
        for (int c = 0; c < grid::kNumCells; ++c)
        {
            const grid::Cell& cell = grid::kCells[c];
            if (cell.kind == grid::Kind::Toggle)
                addParam(mpc::boolParam(cell.id, cell.name, cell.initial != 0));
            else if (cell.kind == grid::Kind::Choice)
                addParam(mpc::choiceParam(cell.id, cell.name,
                                          std::vector<std::string>(cell.table, cell.table + (cell.max - cell.min + 1)),
                                          cell.initial - cell.min));
            else if (isSplit(cell))
            {
                std::vector<std::string> semitones;
                for (int v = -kSemitones; v <= kSemitones; ++v) semitones.push_back(signedText(v));
                addParam(mpc::choiceParam(cell.id, cell.name, semitones, semitonesOf(cell.initial) + kSemitones));
            }
            else
                addParam(mpc::intParam(cell.id, cell.name, cell.min, cell.max, cell.initial));
            cellRelevant_[c].store(true);   // until a preset says otherwise
            snapRelevant_[c].store(true);
            fineOfCell_[c] = -1;
        }
        // The cents of each transpose, after every pad so that the pads' places did not move.
        int numFine = 0;
        for (int c = 0; c < grid::kNumCells && numFine < kNumParams - kFine; ++c)
        {
            const grid::Cell& cell = grid::kCells[c];
            if (!isSplit(cell)) continue;
            // "Osc1 Transp" -> "Osc1 Fine"; the master one, "Transpose", -> "Fine Tune".
            std::string name = cell.name;
            const size_t at = name.find("Transp");
            name = at == 0 ? "Fine Tune" : name.substr(0, at) + "Fine";
            std::vector<std::string> cents;
            for (int v = -50; v <= 49; ++v) cents.push_back(signedText(v));
            addParam(mpc::choiceParam((std::string(cell.id) + "f").c_str(), name.c_str(), cents, centsOf(cell.initial) + 50));
            fineOfCell_[c] = numFine;
            cellOfFine_[numFine++] = c;
        }
        for (int m = 0; m < kNumEngineParams; ++m)
        {
            pending_[m].store(kNoChange);
            host_[m].store(shownValue(m));
            lastHostChange_[m].store(0);
            snap_[m].store(0);
        }
        for (auto& word : hostToTell_) word.store(0);
    }

    ~DelugeSynth() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        workerRunning_.store(false);
        const int instance = engineInstance_.exchange(-1);
        if (instance >= 0)
        {
            EngineHold engine(instance);
            deluge_hosted::engineDeleteInstance(instance);
        }
    }

    // Not the audio thread. The MPC calls this before processing starts and after every
    // suspend/resume.
    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)sampleRate; (void)maxBlockSize;   // the MPC and the Deluge both run at 44100 Hz
        if (engineInstance_.load() < 0 && !triedForInstance_)
        {
            triedForInstance_ = true;
            EngineHold engine(-1);
            deluge_hosted::engineInit(mpc::contentFolder().c_str());   // only the first call starts it
            engineInstance_.store(deluge_hosted::engineNewInstance());
        }
        if (engineInstance_.load() >= 0 && !worker_.joinable())
        {
            // The first preset is loaded here, before processing starts, so the first notes are
            // not lost while it loads. Later changes go through the worker thread.
            requestedPreset_.store(presetIndex());
            loadRequestedPreset();
            workerRunning_.store(true);
            worker_ = std::thread([this] { workerLoop(); });
        }
        leftoverFrames_ = 0;
        writeDiagnostics();
    }

    void reset() override
    {
        smoothedGain_ = dbToGain(get(kVolume));
        wantAllNotesOff_.store(true);
    }

    // May be called on the audio thread, so only atomics and the preset lists, which never change
    // after the constructor. Browsing is settled here and now: by the time this returns, Preset
    // and Folder hold their final values, so a project saved at any moment has the right preset.
    // What waits for the worker thread: telling the MPC about values we changed (it does not
    // expect to be called back from inside its own call), putting a button back to off, and
    // loading the preset.
    void parameterChanged(int index, float plainValue) override
    {
        const bool pressed = plainValue > 0.5f;
        switch (index)
        {
            case kPreset:     requestedPreset_.store(presetIndex()); followFolder(); break;
            case kFolder:     if (!tSettingFolder) browse(0, 0, mpc::clamp((int)std::lround(plainValue), 0, std::max(0, (int)folders_.size() - 1))); break;
            case kPresetPrev: buttonSet(index, plainValue); if (pressed) browse(-1, 0, -1); break;
            case kPresetNext: buttonSet(index, plainValue); if (pressed) browse(+1, 0, -1); break;
            case kFolderPrev: buttonSet(index, plainValue); if (pressed) browse(0, -1, -1); break;
            case kFolderNext: buttonSet(index, plainValue); if (pressed) browse(0, +1, -1); break;
            default:
                // A gold knob or grid pad moved by the user or the host (not by this thread
                // mirroring the engine): remember it, and process() hands it to the engine.
                if (index >= kGold && index < kNumParams && !tMirroringKnobs)
                {
                    // A transpose's two halves make one engine value; the half that did not move
                    // is taken from the last value known.
                    const int m = index >= kFine ? kFirstCell + cellOfFine_[index - kFine] : index - kGold;
                    int value = (int)std::lround(plainValue);
                    if (index >= kFine)
                        value = mpc::clamp(semitonesOf(host_[m].load()) * 100 + (value - 50), -kSemitones * 100, kSemitones * 100);
                    else if (m >= kFirstCell && fineOfCell_[m - kFirstCell] >= 0)
                        value = mpc::clamp((value - kSemitones) * 100 + centsOf(host_[m].load()), -kSemitones * 100, kSemitones * 100);
                    host_[m].store(value);
                    pending_[m].store(value);
                    // After the value is in place: a reader that sees the new stamp sees the value.
                    lastHostChange_[m].store(changeCounter_.fetch_add(1) + 1);
                    anyPending_.store(true);
                }
                break;
        }
    }

    // A button is a momentary request. It stays on for a tenth of a second, so that the MPC's
    // screen shows the press (and the SDK's tools, which set it and read it back to find out what
    // kind of parameter it is, see a switch), then the worker thread puts it back to off.
    void buttonSet(int index, float plainValue)
    {
        if (plainValue != 0.0f && !tReleasingButton) buttonOnSince_[index - kFolderPrev].store(nowMs());
    }
    static int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Moves the Preset parameter by a number of presets or folders, or to a folder's first preset.
    void browse(int presetSteps, int folderSteps, int folderSet)
    {
        if (presets_.empty()) return;
        int preset = presetIndex();
        const int before = preset;
        if (presetSteps != 0) preset = mpc::clamp(preset + presetSteps, 0, (int)presets_.size() - 1);
        if (folderSteps != 0)
            preset = folderStart_[(size_t)mpc::clamp(folderIndexOfPreset(preset) + folderSteps, 0, (int)folders_.size() - 1)];
        if (folderSet >= 0 && folderSet != folderIndexOfPreset(preset)) preset = folderStart_[(size_t)folderSet];
        if (preset != before) set(kPreset, (float)preset);   // comes back through parameterChanged(kPreset)
        else followFolder();
        browseChanged_.store(true);
    }

    // Folder always shows the folder of the selected preset.
    void followFolder()
    {
        if (presets_.empty()) return;
        const float folder = (float)folderIndexOfPreset(presetIndex());
        if (get(kFolder) != folder)
        {
            tSettingFolder = true;
            set(kFolder, folder);
            tSettingFolder = false;
            browseChanged_.store(true);
        }
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index == kPreset)
        {
            if (presets_.empty()) { out = "(none)"; return true; }
            const std::string& path = presets_[(size_t)mpc::clamp((int)std::lround(plainValue), 0, (int)presets_.size() - 1)];
            const size_t slash = path.rfind('/');
            const size_t start = slash == std::string::npos ? 0 : slash + 1;
            out = path.substr(start, path.size() - start - 4);   // file name without folder or ".XML"
            return true;
        }
        if (index >= kGold && index < kGold + deluge_hosted::kNumKnobs)
        {
            // "LPF frequency 25": the preset's name for the knob, and its position as the Deluge
            // shows it (0 to 50).
            const char* name = knobNames_[index - kGold];
            if (name[0] == 0) { out = "-"; return true; }
            char text[48];
            std::snprintf(text, sizeof text, "%.22s %d", name, ((int)std::lround(plainValue) + 64) * 50 / 128);
            out = text;
            return true;
        }
        if (index >= kGrid && index < kFine)
        {
            // The firmware's own text for the value (grid_table.h), or "-" for a pad that does
            // nothing in the loaded preset.
            const int c = index - kGrid;
            const grid::Cell& cell = grid::kCells[c];
            if (!cellRelevant_[c].load(std::memory_order_relaxed)) { out = "-"; return true; }
            if (fineOfCell_[c] >= 0) { out = signedText(mpc::clamp((int)std::lround(plainValue), 0, 2 * kSemitones) - kSemitones); return true; }
            const int value = mpc::clamp((int)std::lround(plainValue), (int)cell.min, (int)cell.max);
            if (cell.text == grid::Text::Table) { out = cell.table[value - cell.min]; return true; }
            for (int e = 0; e < cell.numExceptions; ++e)
                if (cell.exceptions[e].value == value) { out = cell.exceptions[e].text; return true; }
            char text[24];
            if (cell.text == grid::Text::Hundredths) std::snprintf(text, sizeof text, "%.2f", (double)value / 100.0);
            else std::snprintf(text, sizeof text, "%d", value);
            out = text;
            return true;
        }
        if (index >= kFine && index < kNumParams)
        {
            if (!cellRelevant_[cellOfFine_[index - kFine]].load(std::memory_order_relaxed)) { out = "-"; return true; }
            out = signedText(mpc::clamp((int)std::lround(plainValue), 0, 99) - 50);
            return true;
        }
        if (index == kFolder)
        {
            if (folders_.empty()) { out = "(none)"; return true; }
            const std::string& f = folders_[(size_t)mpc::clamp((int)std::lround(plainValue), 0, (int)folders_.size() - 1)];
            out = f.empty() ? "(top)" : f;
            return true;
        }
        return false;
    }

    // The project remembers the preset by its path under Content/SYNTHS, not by its position in
    // the list, so adding or removing preset files does not change what a project plays.
    //
    // Format: the path, then optionally a zero byte and flags. The one flag so far, 'K', says the
    // gold values saved alongside do NOT belong to this preset: the save came after a change of
    // preset and before the new preset's knob positions had been taken, so they are still the
    // previous preset's. (A project from before the flag existed has the path alone.)
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        // The parameters have just been written into the project, before this was called.
        statSaves_.fetch_add(1);
        const int i = presetIndex();
        const bool goldIsAnotherPresets = goldPreset_.load() != i;
        if (i >= 0)
        {
            out.assign(presets_[(size_t)i].begin(), presets_[(size_t)i].end());
            if (goldIsAnotherPresets) { out.push_back(0); out.push_back('K'); }
        }
        // If a preset is on its way in, let it arrive before returning, so that what the MPC
        // looks at next is the settled state.
        waitUntilSettled();
        writeDiagnostics();   // a project save is a convenient moment to refresh the file
    }

    // Runs on the MPC's thread, straight after it has set every parameter from the project.
    void loadExtraState(const uint8_t* data, size_t size) override
    {
        const uint8_t* end = static_cast<const uint8_t*>(std::memchr(data, 0, size));
        const size_t pathSize = end != nullptr ? (size_t)(end - data) : size;
        const bool goldIsThisPresets = !(end != nullptr && std::memchr(end + 1, 'K', size - pathSize - 1) != nullptr);
        const std::string wanted(reinterpret_cast<const char*>(data), pathSize);
        statRestores_.fetch_add(1);
        if (!goldIsThisPresets) statRestoresWithoutKnobs_.fetch_add(1);
        for (size_t i = 0; i < presets_.size(); ++i)
        {
            if (presets_[i] == wanted) { set(kPreset, (float)i); break; }
        }
        // (Not found: the file was removed or renamed. Keep whatever the Preset number selects.)

        if (goldIsThisPresets)
        {
            // The gold values the MPC has just set are the user's settings for this preset: they
            // go to the engine, the preset's own do not replace them. The worker thread may be
            // in the middle of copying some other preset's positions into the parameters: put
            // back what the MPC set (syncKnobs sees knobChanges_ move and does the same).
            goldPreset_.store(presetIndex());
            knobChanges_.fetch_add(1);
            putBackHostKnobs(false);
            // The MPC set the parameters one by one, in order, and some pads only take a value
            // once another has changed (an FM control needs the synth mode set first). Have the
            // worker hand them all to the engine again, in as many rounds as that takes.
            resync_.store(true);
        }
        else
        {
            // Nothing of the user's to keep: load the preset afresh and take its own positions.
            goldPreset_.store(-1);
            reloadPreset_.store(true);
        }
        waitUntilSettled();
    }

    // Makes the engine parameters hold again the values last set from outside (by the MPC).
    void putBackHostKnobs(bool tellHost)
    {
        tMirroringKnobs = true;
        for (int m = 0; m < kNumEngineParams; ++m)
        {
            const int wanted = host_[m].load();
            if (shownValue(m) == wanted) continue;
            show(m, wanted);
            if (tellHost) tellHostOf(m);
        }
        tMirroringKnobs = false;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const auto timeStart = std::chrono::steady_clock::now();
        const int numFrames = ctx.numFrames;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        statCalls_.fetch_add(1, std::memory_order_relaxed);

        // Silence (the outputs are already zero) when this instance has no place in the engine or
        // a worker thread has it (loading a preset, or handing it a restored project's knobs).
        const int instance = engineInstance_.load(std::memory_order_relaxed);
        const bool owner = instance >= 0;
        if (!engineForAudio(instance))
        {
            statSilentCalls_.fetch_add(1, std::memory_order_relaxed);
            // The notes in this block are lost, but a note-off must not be: its note would sound
            // until the next preset change. Keep it for the next block that gets the engine.
            if (owner)
            {
                for (int i = 0; i < ctx.numMidiEvents; ++i)
                {
                    const mpc::MidiEvent& e = ctx.midi[i];
                    if (e.isNoteOff()) missedNoteOff_[(e.note() >> 6) & 1] |= uint64_t(1) << (e.note() & 63);
                    else if (e.isAllNotesOff()) wantAllNotesOff_.store(true);
                }
            }
            return;
        }

        if (wantAllNotesOff_.exchange(false)) deluge_hosted::engineAllNotesOff();
        if ((missedNoteOff_[0] | missedNoteOff_[1]) != 0)
        {
            for (int note = 0; note < 128; ++note)
                if (missedNoteOff_[note >> 6] & (uint64_t(1) << (note & 63))) deluge_hosted::engineNoteOff(note);
            missedNoteOff_[0] = missedNoteOff_[1] = 0;
        }
        // Engine parameters set from outside since the last block. Afterwards everything is read
        // back (not more often than every kSnapshotSpacing frames), because one change can move
        // others: a gold knob and a pad on the same thing, a synth mode that brings pads to life.
        const int changesBefore = changeCounter_.load(std::memory_order_relaxed);
        if (anyPending_.exchange(false, std::memory_order_relaxed))
        {
            for (int m = 0; m < kNumEngineParams; ++m)
            {
                const int value = pending_[m].exchange(kNoChange, std::memory_order_relaxed);
                if (value == kNoChange) continue;
                engineSet(m, value);
                snapshotWanted_ = true;
            }
        }
        framesSinceSnapshot_ = std::min(framesSinceSnapshot_ + numFrames, kSnapshotSpacing);
        if (snapshotWanted_ && framesSinceSnapshot_ >= kSnapshotSpacing)
        {
            snapshotWanted_ = false;
            framesSinceSnapshot_ = 0;
            takeSnapshot(changesBefore);
        }
        const float targetGain = dbToGain(get(kVolume));

        int frame = 0;
        int midiIndex = 0;
        while (frame < numFrames)
        {
            while (midiIndex < ctx.numMidiEvents && ctx.midi[midiIndex].sampleOffset <= frame)
            {
                handleMidi(ctx.midi[midiIndex]);
                ++midiIndex;
            }
            int segmentEnd = numFrames;
            if (midiIndex < ctx.numMidiEvents)
                segmentEnd = mpc::clamp(ctx.midi[midiIndex].sampleOffset, frame + 1, numFrames);

            while (frame < segmentEnd)
            {
                // The engine renders in multiples of four frames, up to 128. Whatever it renders
                // beyond what this segment needs (at most three frames) is kept for the next one.
                if (leftoverFrames_ == 0)
                {
                    const int wanted = std::min(segmentEnd - frame, deluge_hosted::kMaxRenderFrames);
                    const int rendered = (wanted + 3) & ~3;
                    deluge_hosted::engineRender(engineBuffer_, rendered);
                    leftoverFrames_ = rendered;
                    leftoverStart_ = 0;
                }
                const int n = std::min(segmentEnd - frame, leftoverFrames_);
                const int32_t* src = engineBuffer_ + 2 * leftoverStart_;
                // The engine's output stage clips at full scale, as the Deluge's does on the way to
                // its converter. Count the samples that sit on that ceiling.
                uint32_t clipped = 0;
                for (int i = 0; i < 2 * n; ++i) clipped += (src[i] >= 0x7FFFFF00 || src[i] == INT32_MIN) ? 1u : 0u;
                if (clipped != 0) statClipped_.fetch_add(clipped, std::memory_order_relaxed);
                for (int i = 0; i < n; ++i)
                {
                    smoothedGain_ += 0.002f * (targetGain - smoothedGain_);
                    const float scale = smoothedGain_ * (1.0f / 2147483648.0f);
                    outL[frame + i] = (float)src[2 * i] * scale;
                    if (outR) outR[frame + i] = (float)src[2 * i + 1] * scale;
                }
                frame += n;
                leftoverStart_ += n;
                leftoverFrames_ -= n;
            }
        }
        gEngineLock.unlock();

        float peak = statPeak_.load(std::memory_order_relaxed);
        for (int i = 0; i < numFrames; ++i) peak = std::max(peak, std::fabs(outL[i]));
        statPeak_.store(peak, std::memory_order_relaxed);
        const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - timeStart).count();
        statNs_.fetch_add(ns, std::memory_order_relaxed);
        statFrames_.fetch_add((uint64_t)numFrames, std::memory_order_relaxed);
        if (numFrames < statMinBlock_.load(std::memory_order_relaxed)) statMinBlock_.store(numFrames, std::memory_order_relaxed);
        if (numFrames > statMaxBlock_.load(std::memory_order_relaxed)) statMaxBlock_.store(numFrames, std::memory_order_relaxed);
        const uint32_t permille = numFrames > 0 ? (uint32_t)(ns * 441ull / (10000ull * (uint64_t)numFrames)) : 0u;
        if (permille > statWorstPermille_.load(std::memory_order_relaxed)) statWorstPermille_.store(permille, std::memory_order_relaxed);
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Deluge Synth";
        info.vendor       = "Fentonia";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1000;         // 1.0.0.0
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 0;
        return info;
    }

    static constexpr char kUid[5] = "F6wM";

    static float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }

    // Engine lock held.
    static void engineSet(int m, int value)
    {
        if (m < kFirstCell) { deluge_hosted::engineKnobSet(m, value); return; }
        const grid::Cell& cell = grid::kCells[m - kFirstCell];
        deluge_hosted::engineCellSet(cell.layer, cell.x, cell.y, value);
    }

    // Engine lock held; audio thread or worker. Publishes what every engine parameter holds now,
    // for the worker thread to show (showSnapshot). `changes` is changeCounter_ as it was before
    // the values this reflects were handed to the engine.
    void takeSnapshot(int changes)
    {
        int32_t values[deluge_hosted::kGridSlots];
        uint8_t relevant[deluge_hosted::kGridSlots];
        deluge_hosted::engineGridRead(values, relevant);
        for (int c = 0; c < grid::kNumCells; ++c)
        {
            const int slot = slotOf(grid::kCells[c]);
            snapRelevant_[c].store(relevant[slot] != 0, std::memory_order_relaxed);
            if (relevant[slot]) snap_[kFirstCell + c].store((int)values[slot], std::memory_order_relaxed);
        }
        for (int k = 0; k < deluge_hosted::kNumKnobs; ++k)
        {
            int position = 0;
            const bool valid = deluge_hosted::engineKnobGet(k, &position);
            snapKnobValid_[k].store(valid, std::memory_order_relaxed);
            if (valid) snap_[k].store(position, std::memory_order_relaxed);
        }
        snapChanges_.store(changes, std::memory_order_relaxed);
        snapSerial_.fetch_add(1);
        statSnapshots_.fetch_add(1, std::memory_order_relaxed);
    }

    // Preset files under Content/SYNTHS, subfolders included, as paths relative to that folder
    // ("Pads/Warm.XML"), sorted so that a folder's presets sit together. Done once, in the
    // constructor.
    void scanPresets()
    {
        scanFolder(mpc::contentPath("SYNTHS"), "", 0);
        std::sort(presets_.begin(), presets_.end(), lessIgnoringCase);
        if (presets_.size() > (size_t)kMaxPresets) presets_.resize((size_t)kMaxPresets);
        // The folders, in the order their presets appear, and where each one starts in the list.
        for (size_t i = 0; i < presets_.size(); ++i)
        {
            const std::string folder = folderOf(presets_[i]);
            if (folders_.empty() || folders_.back() != folder) { folders_.push_back(folder); folderStart_.push_back((int)i); }
        }
    }

    static std::string folderOf(const std::string& path)
    {
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }

    int folderIndexOfPreset(int preset) const
    {
        int f = 0;
        for (size_t i = 0; i < folderStart_.size(); ++i) if (folderStart_[i] <= preset) f = (int)i;
        return f;
    }

    static bool lessIgnoringCase(const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; }

    void scanFolder(const std::string& absolute, const std::string& relative, int depth)
    {
        DIR* d = opendir(absolute.c_str());
        if (!d) return;
        while (dirent* e = readdir(d))
        {
            const std::string name = e->d_name;
            if (name.empty() || name[0] == '.') continue;
            const std::string childAbsolute = absolute + "/" + name;
            const std::string childRelative = relative.empty() ? name : relative + "/" + name;
            struct stat st;
            if (stat(childAbsolute.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) { if (depth < 6) scanFolder(childAbsolute, childRelative, depth + 1); }
            else if (name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".xml") == 0) presets_.push_back(childRelative);
        }
        closedir(d);
    }

    int presetIndex() { return presets_.empty() ? -1 : mpc::clamp(getInt(kPreset), 0, (int)presets_.size() - 1); }

    // Loads the requested preset whenever it differs from the loaded one. Holding the engine lock
    // makes process() output silence for the duration.
    void workerLoop()
    {
        while (!quit_.load())
        {
            applyBrowsing();
            loadRequestedPreset();
            showSnapshot();
            tellHostNow();
            // Keep the diagnostics file a few seconds fresh while audio runs, so that it says
            // something useful after a crash or a reset too.
            const int64_t now = nowMs();
            if (now - diagnosticsWrittenMs_ >= 3000 && statCalls_.load() != diagnosticsCalls_)
            {
                noteFirmwareErrors();
                diagnosticsWrittenMs_ = now;
                diagnosticsCalls_ = statCalls_.load();
                writeDiagnostics();
            }
            // Someone waiting on us (waitUntilSettled): look again at once.
            std::this_thread::sleep_for(std::chrono::milliseconds(waiters_.load() != 0 ? 1 : 15));
        }
    }

    // Tells the MPC about the parameters parameterChanged() moved, so its screen follows. Runs on
    // the worker thread.
    void applyBrowsing()
    {
        bool changed = browseChanged_.exchange(false);
        if (changed) for (int index : {kPreset, kFolder}) tellHostLater(index);
        const int64_t now = nowMs();
        for (int button = kFolderPrev; button <= kPresetNext; ++button)
        {
            if (get(button) == 0.0f || now - buttonOnSince_[button - kFolderPrev].load() < 100) continue;
            tReleasingButton = true;
            set(button, 0.0f);
            tReleasingButton = false;
            tellHostLater(button);
        }
    }

    // The worker thread never calls the MPC directly while the MPC's own thread may be inside
    // saveExtraState() or loadExtraState() waiting for it (waitUntilSettled): the MPC could be
    // holding something its callbacks need, and each would wait for the other. So what the MPC
    // has to be told is noted here and passed on by the worker loop when nobody is waiting.
    void tellHostLater(int index)
    {
        hostToTell_[index >> 6].fetch_or(uint64_t(1) << (index & 63));
    }
    void tellHostNow()
    {
        if (waiters_.load() != 0) return;
        bool any = false;
        for (int word = 0; word < kTellWords; ++word)
        {
            const uint64_t which = hostToTell_[word].exchange(0);
            if (which == 0) continue;
            any = true;
            for (int bit = 0; bit < 64 && word * 64 + bit < numParams(); ++bit)
                if (which & (uint64_t(1) << bit)) notifyHostParameterChanged(word * 64 + bit);
        }
        if (any) requestDisplayUpdate();
    }

    // Worker thread: after the audio thread has read the engine back (takeSnapshot), make the
    // parameters show it. A parameter the MPC has set since then is left alone: its value is
    // newer than the snapshot and on its way to the engine.
    void showSnapshot()
    {
        const uint32_t serial = snapSerial_.load();
        if (serial == snapSeen_) return;
        if (waiters_.load() != 0 || knobsBusy_.load()) return;   // a project save or load is in hand: afterwards
        snapSeen_ = serial;
        const int loaded = loadedPreset_.load();
        // About to be settled as a whole by syncKnobs(): nothing to add here.
        if (goldPreset_.load() != loaded || requestedPreset_.load() != loaded || reloadPreset_.load() || resync_.load()) return;
        const int changes = snapChanges_.load();
        for (int m = 0; m < kNumEngineParams; ++m)
        {
            if (m >= kFirstCell)
            {
                const bool relevant = snapRelevant_[m - kFirstCell].load();
                if (cellRelevant_[m - kFirstCell].exchange(relevant) != relevant) tellHostOf(m);   // its text changes
                if (!relevant) continue;
            }
            else if (!snapKnobValid_[m].load()) continue;
            if (pending_[m].load() != kNoChange) continue;
            const int stamp = lastHostChange_[m].load();
            if (stamp - changes > 0) continue;
            showEngineValue(m, snap_[m].load(), stamp);
        }
    }

    // Worker thread: parameter m shows `value`, which is what the engine holds, unless the MPC
    // has set the parameter since `stamp` was read; then the MPC's value stands.
    void showEngineValue(int m, int value, int stamp)
    {
        if (shownValue(m) == value) { if (lastHostChange_[m].load() == stamp) host_[m].store(value); return; }
        tMirroringKnobs = true;
        show(m, value);
        if (lastHostChange_[m].load() != stamp) show(m, host_[m].load());
        else { host_[m].store(value); statShown_.fetch_add(1); }
        tMirroringKnobs = false;
        tellHostOf(m);
    }

    // Engine parameter m as the plugin's parameters show it, and making them show a value. One
    // parameter each, with the engine's own number, except a transpose (isSplit): two.
    int shownValue(int m) const
    {
        const int fine = m >= kFirstCell ? fineOfCell_[m - kFirstCell] : -1;
        if (fine < 0) return getInt(kGold + m);
        return mpc::clamp((getInt(kGold + m) - kSemitones) * 100 + (getInt(kFine + fine) - 50), -kSemitones * 100, kSemitones * 100);
    }
    void show(int m, int value)
    {
        const int fine = m >= kFirstCell ? fineOfCell_[m - kFirstCell] : -1;
        if (fine < 0) { set(kGold + m, (float)value); return; }
        set(kGold + m, (float)(semitonesOf(value) + kSemitones));
        set(kFine + fine, (float)(centsOf(value) + 50));
    }
    void tellHostOf(int m)
    {
        tellHostLater(kGold + m);
        if (m >= kFirstCell && fineOfCell_[m - kFirstCell] >= 0) tellHostLater(kFine + fineOfCell_[m - kFirstCell]);
    }
    static std::string signedText(int v)
    {
        char text[8];
        std::snprintf(text, sizeof text, v > 0 ? "+%d" : "%d", v);
        return text;
    }

    // Called on the MPC's thread around a project save or load: gives the worker thread time to
    // finish a preset load and the knob hand-over that follows, so that what is saved, or what the
    // MPC reads back after a load, is the settled state and not a moment in between. Gives up
    // after three seconds (a very large sample preset on a slow card) and carries on.
    void waitUntilSettled()
    {
        if (!workerRunning_.load()) return;
        waiters_.fetch_add(1);
        bool done = false;
        for (int ms = 0; ms < 3000; ++ms)
        {
            const int requested = requestedPreset_.load();
            const bool settled = loadedPreset_.load() == requested && goldPreset_.load() == requested
                                 && !reloadPreset_.load() && !resync_.load() && !knobsBusy_.load();
            if (settled || quit_.load()) { done = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!done) statWaitTimeouts_.fetch_add(1);
        waiters_.fetch_sub(1);
    }

    void loadRequestedPreset()
    {
        const int wanted = requestedPreset_.load();
        if (wanted < 0 || wanted >= (int)presets_.size()) return;
        const bool reload = reloadPreset_.exchange(false);
        if (wanted == loadedPreset_.load() && !reload)
        {
            // Nothing to load. But the engine parameters may still be another preset's (the user
            // went to a different preset and came back while this one was on its way in), or a
            // project has just set them all and they have to go to the engine together.
            if (goldPreset_.load() != wanted || resync_.load())
            {
                EngineHold engine(engineInstance_.load());
                resync_.store(false);
                syncKnobs();
            }
            return;
        }
        const std::string path = "SYNTHS/" + presets_[(size_t)wanted];
        EngineHold engine(engineInstance_.load());
        resync_.store(false);
        loadError_.store(deluge_hosted::engineLoadSynth(path.c_str()));
        int samplesLoaded = 0, samplesFailed = 0;
        deluge_hosted::engineSampleCounts(&samplesLoaded, &samplesFailed);
        samplesLoaded_.store(samplesLoaded);
        samplesFailed_.store(samplesFailed);
        loadedPreset_.store(wanted);
        loads_.fetch_add(1);
        syncKnobs();
        noteFirmwareErrors();
    }

    // Worker thread. Needs no engine lock: it reads a counter and a few characters.
    void noteFirmwareErrors()
    {
        firmwareErrors_.store(deluge_hosted::engineFirmwareErrors(latestFirmwareError_, (int)sizeof latestFirmwareError_));
    }

    // With a preset in the engine (engine lock held): make the engine parameters and the engine
    // agree. goldPreset_ says which preset the parameter values belong to:
    //
    //   the loaded one     they are the user's (set on the MPC, or restored with a project) and
    //                      go to the engine
    //   another, and the   the user has changed preset: the loaded preset's own knob positions
    //   loaded one is the  and pad values become the parameter values, and from now on belong
    //   one asked for      to it
    //   another, and the   this preset was already on its way in when the user chose again; it
    //   user has moved on  is about to be replaced, so nothing is copied either way
    void syncKnobs()
    {
        const int loaded = loadedPreset_.load();
        const bool keep = goldPreset_.load() == loaded;
        const bool adopt = !keep && loaded == requestedPreset_.load();
        const int changesBefore = knobChanges_.load();
        knobsBusy_.store(true);
        tMirroringKnobs = true;
        for (int k = 0; k < deluge_hosted::kNumKnobs; ++k)
        {
            deluge_hosted::engineKnobName(k, knobNames_[k], (int)sizeof knobNames_[k]);
            int position = 0;
            if (keep)
            {
                pending_[k].store(kNoChange);
                deluge_hosted::engineKnobSet(k, getInt(kGold + k));
            }
            else if (adopt && deluge_hosted::engineKnobGet(k, &position))
            {
                pending_[k].store(kNoChange);
                if (getInt(kGold + k) != position) { set(kGold + k, (float)position); tellHostLater(kGold + k); }
                host_[k].store(position);
            }
        }
        tMirroringKnobs = false;

        // The grid pads. Kept values go to the engine only where the engine holds something
        // else (after the gold knobs above, which may reach the same things), and in rounds:
        // setting one pad (the synth mode, an oscillator's type) can bring others to life, which
        // then take their values in the next round.
        int32_t values[deluge_hosted::kGridSlots];
        uint8_t relevant[deluge_hosted::kGridSlots];
        if (keep)
        {
            for (int c = 0; c < grid::kNumCells; ++c) pending_[kFirstCell + c].store(kNoChange);
            // A round offers every pad that applies its kept value where the engine holds another.
            // Rounds go on while one changes anything in the engine: a value the firmware will
            // not hold (an oscillator type that ring-mod mode does not have) is offered again once
            // something else (the mode) has changed, and no more when nothing does.
            int32_t before[deluge_hosted::kGridSlots];
            uint8_t relevantBefore[deluge_hosted::kGridSlots];
            deluge_hosted::engineGridRead(before, relevantBefore);
            for (int round = 0; round < 8; ++round)
            {
                for (int c = 0; c < grid::kNumCells; ++c)
                {
                    const grid::Cell& cell = grid::kCells[c];
                    const int slot = slotOf(cell);
                    if (!relevantBefore[slot] || before[slot] == shownValue(kFirstCell + c)) continue;
                    deluge_hosted::engineCellSet(cell.layer, cell.x, cell.y, shownValue(kFirstCell + c));
                }
                deluge_hosted::engineGridRead(values, relevant);
                bool changed = false;
                for (int c = 0; c < grid::kNumCells && !changed; ++c)
                {
                    const int slot = slotOf(grid::kCells[c]);
                    changed = relevant[slot] != relevantBefore[slot] || (relevant[slot] && values[slot] != before[slot]);
                }
                if (!changed) break;
                std::memcpy(before, values, sizeof before);
                std::memcpy(relevantBefore, relevant, sizeof relevantBefore);
            }
        }
        // Then the parameters show what the engine holds: all of the new preset's values, or,
        // of kept ones, the few the firmware held differently.
        deluge_hosted::engineGridRead(values, relevant);
        for (int c = 0; c < grid::kNumCells; ++c)
        {
            const int slot = slotOf(grid::kCells[c]);
            const bool nowRelevant = relevant[slot] != 0;
            if (cellRelevant_[c].exchange(nowRelevant) != nowRelevant) tellHostOf(kFirstCell + c);
            if (!nowRelevant || !(keep || adopt)) continue;
            if (adopt) pending_[kFirstCell + c].store(kNoChange);
            showEngineValue(kFirstCell + c, (int)values[slot], lastHostChange_[kFirstCell + c].load());
        }
        snapSeen_ = snapSerial_.load();   // an earlier read-back is of the engine as it was before this

        if (adopt)
        {
            if (knobChanges_.load() == changesBefore) goldPreset_.store(loaded);
            else
            {
                // (Known gap: a parameter the restore set before the loops above reached it has
                // been overwritten there, host_ included, and comes back as the preset's own
                // value. It needs a restore to land while a preset is still on its way in; the
                // SDK simulator, which restores straight after choosing a random preset, shows it
                // about once in a hundred runs. See CLAUDE.md.)
                // A project restore arrived while the loops above were running, and wins: the
                // values it set are in host_. If they are for this preset they also have to reach
                // the engine (the next turn of the worker loop does it); if for another, that
                // preset's load will see to it.
                putBackHostKnobs(true);
                if (goldPreset_.load() == loaded) resync_.store(true);
            }
        }
        knobsBusy_.store(false);
        tellHostLater(kPreset);   // the knob names changed: have the MPC read the texts again
    }

    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn()) { deluge_hosted::engineNoteOn(e.note(), e.velocity()); statNoteOns_.fetch_add(1, std::memory_order_relaxed); }
        else if (e.isNoteOff()) deluge_hosted::engineNoteOff(e.note());
        else if (e.isAllNotesOff()) deluge_hosted::engineAllNotesOff();
        else if (e.isPitchBend()) deluge_hosted::enginePitchBend(e.pitchBend());
        else if (e.isController() && e.controller() == 1) deluge_hosted::engineModWheel(e.data[2]);
        else if (e.isAftertouch()) deluge_hosted::engineChannelPressure(e.data[1]);
    }

    // What happened so far, written into the plugin's own folder: there is no other way to see
    // inside the plugin on the device. `mpc_remote.py export` brings the folder back.
    int relevantCells()
    {
        int n = 0;
        for (int c = 0; c < grid::kNumCells; ++c) n += cellRelevant_[c].load() ? 1 : 0;
        return n;
    }

    // The first instance writes diagnostics.txt, the others diagnostics-2.txt and so on.
    std::string diagnosticsName() const
    {
        const int instance = engineInstance_.load();
        return instance <= 1 ? "/diagnostics.txt" : "/diagnostics-" + std::to_string(instance) + ".txt";
    }

    void writeDiagnostics()
    {
        const std::string folder = mpc::pluginFolder();
        if (folder.empty()) return;
        std::lock_guard<std::mutex> lock(diagnosticsLock_);   // the MPC's thread and the worker both write it
        char text[2100];
        const uint64_t frames = statFrames_.load(), ns = statNs_.load();
        const double seconds = (double)frames / 44100.0;
        const int loaded = loadedPreset_.load();
        std::snprintf(text, sizeof text,
                      "Deluge Synth diagnostics (written on prepare, on project/preset save, and every 3 s while audio runs)\n"
                      "engine: %s build; this is instance %d in it (-1: none, too many instances)\n"
                      "presets found in Content/SYNTHS: %d\n"
                      "loaded preset: %d (%s), load error %d, loads so far %d\n"
                      "its samples: %d held in memory, %d could not be loaded (looked for under Content/, e.g. Content/SAMPLES/...)\n"
                      "process calls %u, of which silent (no engine or loading) %u; note-ons %u; peak out %.4f\n"
                      "time in process(): %.2f s of audio rendered, average %.2f%% of one core, worst block %.1f%%\n"
                      "block sizes asked for: %d to %d frames\n"
                      "samples at the engine's clipping ceiling: %u of %llu (%.3f%%)\n"
                      "project saves %u, restores %u (%u of them without usable knob values); waits that timed out %u\n"
                      "grid: %d of %d pads apply to this preset; engine read back %u times, %u parameter values changed to match\n"
                      "firmware error stops (its own consistency checks; should be 0): %d%s%s\n",
#if defined(DELUGE_ENGINE_LINKED)
#if defined(__aarch64__)
                      "Deluge firmware, 64-bit ARM",
#elif defined(__arm__)
                      "Deluge firmware, 32-bit ARM",
#else
                      "Deluge firmware",
#endif
#else
                      "PLACEHOLDER TONE (the firmware is not in this build)",
#endif
                      engineInstance_.load(),
                      (int)presets_.size(), loaded, loaded >= 0 && loaded < (int)presets_.size() ? presets_[(size_t)loaded].c_str() : "none",
                      loadError_.load(), loads_.load(), samplesLoaded_.load(), samplesFailed_.load(), statCalls_.load(), statSilentCalls_.load(), statNoteOns_.load(),
                      (double)statPeak_.load(), seconds, seconds > 0 ? 100.0 * ((double)ns * 1e-9) / seconds : 0.0,
                      (double)statWorstPermille_.load() * 0.1,
                      statMinBlock_.load() == INT32_MAX ? 0 : statMinBlock_.load(), statMaxBlock_.load(),
                      statClipped_.load(), (unsigned long long)(2 * frames),
                      frames > 0 ? 100.0 * (double)statClipped_.load() / (double)(2 * frames) : 0.0,
                      statSaves_.load(), statRestores_.load(), statRestoresWithoutKnobs_.load(), statWaitTimeouts_.load(),
                      relevantCells(), grid::kNumCells, statSnapshots_.load(), statShown_.load(),
                      firmwareErrors_.load(), firmwareErrors_.load() != 0 ? ", latest " : "", latestFirmwareError_);
        if (FILE* f = std::fopen((folder + diagnosticsName()).c_str(), "w"))
        {
            std::fputs(text, f);
            std::fputs("gold knobs (name, position -64..64):", f);
            for (int k = 0; k < deluge_hosted::kNumKnobs; ++k)
                std::fprintf(f, "%s %d=[%s] %d", k % 4 == 0 ? "\n " : ",", k + 1, knobNames_[k], getInt(kGold + k));
            std::fputs("\n", f);
            std::fclose(f);
        }
    }

    std::vector<std::string> presets_;
    std::vector<std::string> folders_;       // distinct folders of presets_, in list order
    std::vector<int>         folderStart_;   // index in presets_ of each folder's first preset
    std::atomic<bool> browseChanged_{false};   // parameterChanged moved Preset, Folder or a button: tell the MPC
    // Per engine parameter (gold knobs, then grid pads):
    static constexpr int kNoChange = -100000;
    int               fineOfCell_[grid::kNumCells];        // which kFine parameter holds a pad's cents, or -1 (constructor only writes)
    int               cellOfFine_[kNumParams - kFine] = {};
    std::atomic<int>  pending_[kNumEngineParams];          // set from outside, not yet given to the engine
    std::atomic<int>  host_[kNumEngineParams];             // the last value set from outside
    std::atomic<int>  lastHostChange_[kNumEngineParams];   // changeCounter_ when it was last set from outside
    std::atomic<int>  snap_[kNumEngineParams];             // what the engine held at the last read-back
    std::atomic<int>  changeCounter_{0};                   // counts every setting from outside
    std::atomic<bool> anyPending_{false};
    std::atomic<bool> snapKnobValid_[deluge_hosted::kNumKnobs];   // at the last read-back: the knob controls something
    std::atomic<bool> snapRelevant_[grid::kNumCells];             // at the last read-back: the pad applies to the preset
    std::atomic<bool> cellRelevant_[grid::kNumCells];             // the same, as the parameters' texts show it
    std::atomic<int>  snapChanges_{0};       // changeCounter_ before the values the read-back reflects went in
    std::atomic<uint32_t> snapSerial_{0};    // counts read-backs
    uint32_t          snapSeen_ = 0;         // worker thread only: the read-back the parameters last followed
    static constexpr int kSnapshotSpacing = 1024;   // frames between read-backs, at least
    bool              snapshotWanted_ = false;      // audio thread only
    int               framesSinceSnapshot_ = kSnapshotSpacing;   // audio thread only
    std::atomic<bool> resync_{false};        // a project set the engine parameters: hand them all to the engine
    char              knobNames_[deluge_hosted::kNumKnobs][32];
    // Set while this thread is the one writing a parameter, so parameterChanged() can tell our own
    // writes from the MPC's. Per thread: the MPC may be setting parameters on another at the same time.
    static inline thread_local bool tMirroringKnobs = false;
    static inline thread_local bool tSettingFolder = false;
    static inline thread_local bool tReleasingButton = false;
    std::atomic<int64_t> buttonOnSince_[4];   // when each of the four buttons last went on, in ms
    // The preset the values of the gold parameters belong to (see syncKnobs). -1: none yet, so
    // the first preset loaded brings its own knob positions.
    std::atomic<int>  goldPreset_{-1};
    std::atomic<int>  knobChanges_{0};       // counts project restores, so syncKnobs can tell one arrived
    std::atomic<bool> knobsBusy_{false};     // the worker is copying knob positions between engine and parameters
    std::atomic<bool> reloadPreset_{false};  // load the requested preset even if it is the one loaded
    static constexpr int kTellWords = (kNumParams + 63) / 64;
    std::atomic<uint64_t> hostToTell_[kTellWords];   // parameters (one bit each) whose new value the MPC has yet to be told
    std::atomic<int>  waiters_{0};           // threads inside waitUntilSettled()
    std::atomic<bool> workerRunning_{false};
    std::thread       worker_;
    std::atomic<bool> quit_{false};
    std::atomic<int>  requestedPreset_{-1}, loadedPreset_{-1}, loadError_{0}, loads_{0};
    std::atomic<int>  samplesLoaded_{0}, samplesFailed_{0};
    std::atomic<bool> wantAllNotesOff_{false};
    uint64_t          missedNoteOff_[2] = {0, 0};   // audio thread only: note-offs that arrived while the engine was busy

    int32_t engineBuffer_[deluge_hosted::kMaxRenderFrames * 2];
    int     leftoverFrames_ = 0, leftoverStart_ = 0;
    float   smoothedGain_ = 1.0f;

    std::atomic<uint32_t> statCalls_{0}, statSilentCalls_{0}, statNoteOns_{0}, statWorstPermille_{0};
    std::atomic<uint64_t> statNs_{0}, statFrames_{0};
    std::atomic<float>    statPeak_{0.0f};
    std::atomic<uint32_t> statClipped_{0}, statSaves_{0}, statRestores_{0}, statRestoresWithoutKnobs_{0}, statWaitTimeouts_{0};
    std::atomic<uint32_t> statSnapshots_{0}, statShown_{0};
    std::atomic<int>      firmwareErrors_{0};
    char                  latestFirmwareError_[8] = {0};   // worker thread writes; read for the diagnostics file
    std::atomic<int>      statMinBlock_{INT32_MAX}, statMaxBlock_{0};
    std::atomic<int>      engineInstance_{-1};          // this plugin instance's place in the engine (engine.h)
    bool                  triedForInstance_ = false;    // prepare() only
    std::mutex            diagnosticsLock_;
    int64_t               diagnosticsWrittenMs_ = 0;   // worker thread only
    uint32_t              diagnosticsCalls_ = 0;       // worker thread only
};

MPC_PLUGIN_ENTRY(DelugeSynth)
