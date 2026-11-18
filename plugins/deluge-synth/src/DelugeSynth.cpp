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

#include "cable_table.h"
#include "cables.h"
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
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

enum ParamIndex
{
    kVolume = 0,     // dB. First on purpose: the MPC nudges parameter 0 on every load.
    kPreset,         // the preset's position in its folder (see presetIndex())
    kFolder,         // index of the folder the preset is in; setting it jumps to that folder's first preset
    kFolderPrev,     // momentary buttons: the plugin acts on a press and puts the button back
    kFolderNext,
    kPresetPrev,
    kPresetNext,
    kGold,           // first of 16: the preset's gold-knob assignments, in the Deluge's order
    kGrid = kGold + deluge_hosted::kNumKnobs,   // first of the shortcut-grid pads, in grid_table.h's order
    kFine = kGrid + grid::kNumCells,            // the cents halves of the transposes (see isSplit), in grid order
    // The oscillators' sample files: for each of the two, in this order, Folder, File, and the
    // four buttons Folder -, Folder +, File -, File +.
    kSample = kFine + 5,
    kSave = kSample + 12,   // a button: write the sound as it stands to a new Deluge preset file
    kSaveResult,            // shows the name it was saved under, or why not
    // The mod matrix: kCableSlots patch cables, each Source, Destination, Amount and Of (below).
    kCable,
    // Two more buttons for MAIN, added later: ten presets back, ten on.
    kPresetBack10 = kCable + 24 * 4,
    kPresetOn10,
    kSampleState,           // shows whether the preset's sample files were found
    // Sidechain steps: one bar of sixteenths, each on or off. While the MPC plays, a step that
    // is on gives the engine a sidechain hit as the bar reaches it (see process()).
    kScStep,
    kNumParams = kScStep + 16
};

// "Engine parameters": the parameters that mirror a value held by the engine, which are the gold
// knobs and then the grid pads. Engine parameter m is plugin parameter kGold + m.
constexpr int kNumEngineParams = deluge_hosted::kNumKnobs + grid::kNumCells;
constexpr int kFirstCell = deluge_hosted::kNumKnobs;   // engine parameter of grid::kCells[0]
constexpr int kNumFine = kSample - kFine;

// A cable slot's four parameters. Source, Destination and Of are positions in cable_table.h's
// lists plus one, 0 meaning none; Of, when set, makes it a cable onto another cable's depth: the
// one from the Of source to the same destination (cables.h). Amount is -5000..5000, shown as
// -50.00..50.00. A slot with no Source or no Destination, or an Amount of 0, is no cable in the
// engine; the slot keeps what was chosen all the same, so a cable can be set up a part at a time
// and turned to nothing without losing its place.
constexpr int kCableSlots = (kPresetBack10 - kCable) / 4;
enum { kCSource = 0, kCDestination, kCAmount, kCOf };
struct CableSlot
{
    int field[4] = {0, 0, 0, 0};
    bool live() const { return field[kCSource] != 0 && field[kCDestination] != 0; }
    bool sameCable(const CableSlot& o) const
    {
        return field[kCSource] == o.field[kCSource] && field[kCDestination] == o.field[kCDestination] && field[kCOf] == o.field[kCOf];
    }
};
enum { kSFolder = 0, kSFile, kSFolderPrev, kSFolderNext, kSFilePrev, kSFileNext, kSParams };

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
        // Preset is the position within the folder that Folder selects, not in the whole list
        // (Jamie, 2026-11-16): an MPC moves these a hundredth of their range per click of a
        // Q-Link or the data wheel, which over 800 presets was eight at a time. Its range is at
        // least 0..100, of which a click is exactly one preset; positions past the end of a
        // folder are its last preset. (A project stores the preset's path, so the ranges may
        // differ from one start to the next.)
        int largestFolder = 1;
        for (size_t f = 0; f < folderStart_.size(); ++f) largestFolder = std::max(largestFolder, folderSize((int)f));
        addParam(mpc::intParam("preset", "Preset", 0, std::max(100, largestFolder - 1), 0));
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
        for (int c = 0; c < grid::kNumCells && numFine < kNumFine; ++c)
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
        // The sample file of each oscillator, chosen from Content/SAMPLES as a preset is from
        // Content/SYNTHS. File 0 is "none of these"; file n is sampleFiles_[n - 1].
        scanSamples();
        for (int s = 0; s < 2; ++s)
        {
            const std::string id = "s" + std::to_string(s + 1), name = "S" + std::to_string(s + 1);
            addParam(mpc::intParam((id + "folder").c_str(), (name + " Folder").c_str(), 0, std::max(0, (int)sampleFolders_.size() - 1), 0));
            addParam(mpc::intParam((id + "file").c_str(), (name + " File").c_str(), 0, (int)sampleFiles_.size(), 0));
            addParam(mpc::boolParam((id + "folderprev").c_str(), (name + " Folder -").c_str(), false));
            addParam(mpc::boolParam((id + "foldernext").c_str(), (name + " Folder +").c_str(), false));
            addParam(mpc::boolParam((id + "fileprev").c_str(), (name + " File -").c_str(), false));
            addParam(mpc::boolParam((id + "filenext").c_str(), (name + " File +").c_str(), false));
            wantedSample_[s].store(kNoChange);
        }
        addParam(mpc::boolParam("save", "Save", false));
        addParam(mpc::intParam("saveresult", "Saved As", 0, 999, 0));
        {
            std::vector<std::string> sources{"-"}, destinations{"-"};
            for (const char* name : cables::kSources) sources.push_back(name);
            for (const char* name : cables::kDestinations) destinations.push_back(name);
            for (int slot = 0; slot < kCableSlots; ++slot)
            {
                const std::string id = "c" + std::to_string(slot + 1), name = "C" + std::to_string(slot + 1);
                addParam(mpc::choiceParam((id + "src").c_str(), (name + " Source").c_str(), sources, 0));
                addParam(mpc::choiceParam((id + "dst").c_str(), (name + " Dest").c_str(), destinations, 0));
                addParam(mpc::intParam((id + "amt").c_str(), (name + " Amount").c_str(), -deluge_hosted::kCableAmountMax, deluge_hosted::kCableAmountMax, 0));
                addParam(mpc::choiceParam((id + "of").c_str(), (name + " Of").c_str(), sources, 0));
                for (int f = 0; f < 4; ++f) { cableHost_[slot][f].store(0); snapSlot_[slot][f].store(0); }
                cableStamp_[slot].store(0);
            }
        }
        addParam(mpc::boolParam("presetback10", "Preset -10", false));
        addParam(mpc::boolParam("preseton10", "Preset +10", false));
        addParam(mpc::intParam("samplestate", "Samples", 0, 999, 0));
        for (int step = 0; step < 16; ++step)
        {
            char id[16], name[16];
            std::snprintf(id, sizeof id, "scstep%d", step + 1);
            std::snprintf(name, sizeof name, "SC Step %d", step + 1);
            addParam(mpc::boolParam(id, name, false));
        }
        std::snprintf(saveText_[0], sizeof saveText_[0], "-");
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
            case kPreset:     if (!tSettingPreset) { requestedPreset_.store(presetIndex()); presetTurnedMs_.store(nowMs()); browseChanged_.store(true); } break;
            // Another folder: its first preset.
            case kFolder:
                if (!tSettingFolder && !presets_.empty())
                    selectPreset(folderStart_[(size_t)mpc::clamp((int)std::lround(plainValue), 0, (int)folders_.size() - 1)]);
                break;
            case kPresetPrev: buttonSet(index, plainValue); if (pressed) browse(-1, 0, -1); break;
            case kPresetNext: buttonSet(index, plainValue); if (pressed) browse(+1, 0, -1); break;
            case kFolderPrev: buttonSet(index, plainValue); if (pressed) browse(0, -1, -1); break;
            case kFolderNext: buttonSet(index, plainValue); if (pressed) browse(0, +1, -1); break;
            case kPresetBack10: buttonSet(index, plainValue); if (pressed) browse(-10, 0, -1); break;
            case kPresetOn10:   buttonSet(index, plainValue); if (pressed) browse(+10, 0, -1); break;
            default:
                // A gold knob or grid pad moved by the user or the host (not by this thread
                // mirroring the engine): remember it, and process() hands it to the engine.
                if (index == kSave) { buttonSet(index, plainValue); if (pressed && !tReleasingButton) saveRequested_.store(true); break; }
                if (index == kSaveResult || index == kSampleState) break;
                if (index >= kCable && index < kPresetBack10)
                {
                    if (tMirroringKnobs) break;   // the worker showing what the engine holds
                    const int slot = (index - kCable) / 4;
                    cableHost_[slot][(index - kCable) % 4].store((int)std::lround(plainValue));
                    cableStamp_[slot].store(changeCounter_.fetch_add(1) + 1);
                    cableDirty_.fetch_or(uint32_t(1) << slot);
                    break;
                }
                if (index >= kSample && index < kSave) { sampleParameterChanged(index, plainValue); break; }
                if (index >= kGold && index < kSample && !tMirroringKnobs)
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
        if (plainValue != 0.0f && !tReleasingButton) buttonOnSince_[buttonSlot(index)].store(nowMs());
    }
    // The buttons: four on MAIN, then four for each oscillator's sample.
    static constexpr int kNumButtons = 15;
    static int buttonSlot(int index)
    {
        if (index == kPresetBack10 || index == kPresetOn10) return 13 + (index - kPresetBack10);
        if (index == kSave) return 12;
        if (index < kSample) return index - kFolderPrev;
        return 4 + ((index - kSample) / kSParams) * 4 + ((index - kSample) % kSParams - kSFolderPrev);
    }
    static int buttonIndex(int slot)
    {
        if (slot >= 13) return kPresetBack10 + (slot - 13);
        if (slot == 12) return kSave;
        if (slot < 4) return kFolderPrev + slot;
        return kSample + ((slot - 4) / 4) * kSParams + kSFolderPrev + (slot - 4) % 4;
    }

    // --- the mod matrix ---------------------------------------------------------------------------
    // slots_ is what the plugin takes each slot to hold; it belongs to whoever holds the engine
    // lock (process(), or the worker inside an EngineHold). The engine has only cables, in an
    // order of its own that changes as they come and go, so a slot is matched to its cable by
    // what it connects, never by position.
    //
    // Engine lock held. Slots the MPC has changed go to the engine. Returns whether any did.
    bool applyCableSlots()
    {
        const uint32_t dirty = cableDirty_.exchange(0);
        for (int slot = 0; slot < kCableSlots; ++slot)
        {
            if (!(dirty & (uint32_t(1) << slot))) continue;
            CableSlot now;
            for (int f = 0; f < 4; ++f) now.field[f] = cableHost_[slot][f].load();
            const CableSlot was = slots_[slot];
            // Another source, destination or Of is another cable: the old one goes.
            if (!now.sameCable(was) && was.live() && was.field[kCAmount] != 0) engineCable(was, 0);
            if (now.live()) engineCable(now, now.field[kCAmount]);
            slots_[slot] = now;
        }
        return dirty != 0;
    }
    static void engineCable(const CableSlot& slot, int amount)
    {
        deluge_hosted::engineCableSet(slot.field[kCSource] - 1, slot.field[kCDestination] - 1, slot.field[kCOf] - 1, amount);
    }
    // Engine lock held. Makes slots_ agree with the engine's cables: a slot whose cable is there
    // takes its amount, one whose cable is gone reads 0, and a cable no slot has (the preset's
    // own, or one a gold knob or the Vibrato pad made) takes the first empty slot. `fresh`
    // empties every slot first: a new preset fills them in the engine's order. The result is
    // left for the worker in snapSlot_.
    void reconcileCables(bool fresh)
    {
        deluge_hosted::Cable list[deluge_hosted::kMaxCables];
        const int count = std::min(deluge_hosted::engineCableList(list, deluge_hosted::kMaxCables), (int)deluge_hosted::kMaxCables);
        bool found[kCableSlots] = {};
        if (fresh) for (CableSlot& slot : slots_) slot = CableSlot();
        int hidden = 0;
        for (int c = 0; c < count; ++c)
        {
            CableSlot cable;
            cable.field[kCSource] = list[c].source + 1;
            cable.field[kCDestination] = list[c].destination + 1;
            cable.field[kCOf] = list[c].of + 1;
            cable.field[kCAmount] = list[c].amount;
            int slot = -1, empty = -1;
            for (int i = 0; i < kCableSlots; ++i)
            {
                if (!found[i] && slots_[i].sameCable(cable)) { slot = i; break; }
                if (empty < 0 && slots_[i].field[kCSource] == 0 && slots_[i].field[kCDestination] == 0 && slots_[i].field[kCOf] == 0) empty = i;
            }
            if (slot < 0) slot = empty;
            if (slot < 0) { ++hidden; continue; }   // more cables than slots: it plays, unseen
            slots_[slot] = cable;
            found[slot] = true;
        }
        for (int i = 0; i < kCableSlots; ++i)
        {
            if (!found[i]) slots_[i].field[kCAmount] = 0;
            for (int f = 0; f < 4; ++f) snapSlot_[i][f].store(slots_[i].field[f], std::memory_order_relaxed);
        }
        cablesHidden_.store(hidden, std::memory_order_relaxed);
    }
    // Worker thread: the slot parameters show snapSlot_, except a slot the MPC has changed since
    // (`changes` is changeCounter_ before the values the read-back reflects went in).
    void showCables(int changes)
    {
        const uint32_t dirty = cableDirty_.load();
        for (int slot = 0; slot < kCableSlots; ++slot)
        {
            if ((dirty & (uint32_t(1) << slot)) || cableStamp_[slot].load() - changes > 0) continue;
            for (int f = 0; f < 4; ++f)
            {
                const int value = snapSlot_[slot][f].load(), index = kCable + slot * 4 + f;
                cableHost_[slot][f].store(value);
                if (getInt(index) == value) continue;
                tMirroringKnobs = true;
                set(index, (float)value);
                tMirroringKnobs = false;
                tellHostLater(index);
            }
        }
    }
    // Worker thread, engine lock held, in syncKnobs() when the parameters are to be kept (a
    // project restore): the engine gets exactly the cables the slot parameters describe.
    void giveCables()
    {
        cableDirty_.store(0);
        CableSlot want[kCableSlots];
        for (int slot = 0; slot < kCableSlots; ++slot)
            for (int f = 0; f < 4; ++f) { want[slot].field[f] = getInt(kCable + slot * 4 + f); cableHost_[slot][f].store(want[slot].field[f]); }
        deluge_hosted::Cable list[deluge_hosted::kMaxCables];
        const int count = std::min(deluge_hosted::engineCableList(list, deluge_hosted::kMaxCables), (int)deluge_hosted::kMaxCables);
        // Cables on another's depth go first out and last in: they need the other to exist.
        for (int pass = 0; pass < 2; ++pass)
            for (int c = 0; c < count; ++c)
            {
                if ((list[c].of >= 0) != (pass == 0)) continue;
                CableSlot cable;
                cable.field[kCSource] = list[c].source + 1;
                cable.field[kCDestination] = list[c].destination + 1;
                cable.field[kCOf] = list[c].of + 1;
                bool wanted = false;
                for (const CableSlot& w : want) wanted = wanted || (w.live() && w.field[kCAmount] != 0 && w.sameCable(cable));
                if (!wanted) engineCable(cable, 0);
            }
        for (int pass = 0; pass < 2; ++pass)
            for (const CableSlot& w : want)
                if (w.live() && w.field[kCAmount] != 0 && (w.field[kCOf] != 0) == (pass == 1)) engineCable(w, w.field[kCAmount]);
        for (int slot = 0; slot < kCableSlots; ++slot) slots_[slot] = want[slot];
    }

    // --- saving the sound as a Deluge preset ---------------------------------------------------
    // Worker thread. The file goes in Content/SYNTHS/User, so that the collections the presets
    // came from stay as they were, under the preset's name with the lowest number from 2 that
    // is free there ("SYNT008 2.XML"), as a Deluge names a copy; a skin has no way to type a
    // name. The firmware's own writer makes it, so it is the
    // file a Deluge would have saved. It joins the Preset list when the plugin next starts (the
    // list, and the Preset parameter's range, are fixed when the plugin loads).
    void applySave()
    {
        if (!saveRequested_.load()) return;
        const int loaded = loadedPreset_.load();
        if (loaded < 0 || loaded != requestedPreset_.load() || goldPreset_.load() != loaded || resync_.load() || reloadPreset_.load()) return;   // once it has settled
        saveRequested_.store(false);
        std::string stem = presets_[(size_t)loaded].substr(0, presets_[(size_t)loaded].size() - 4);   // without ".XML"
        stem = "User/" + stem.substr(stem.rfind('/') == std::string::npos ? 0 : stem.rfind('/') + 1);
        mkdir((mpc::contentPath("SYNTHS") + "/User").c_str(), 0777);   // there already, as a rule
        // A copy of a copy keeps counting: "Name 2" saves as "Name 3".
        const size_t space = stem.rfind(' ');
        if (space != std::string::npos && space + 1 < stem.size() && stem.find_first_not_of("0123456789", space + 1) == std::string::npos)
            stem.resize(space);
        std::string relative;
        int error = -2;   // no free name
        // The SDK's simulator presses every button it finds; build.sh sets this so that a build
        // does not leave preset files behind.
        const bool off = std::getenv("DELUGE_SYNTH_NO_SAVE") != nullptr;
        if (off) error = -3;
        for (int n = 2; n < 1000 && !off; ++n)
        {
            relative = stem + " " + std::to_string(n) + ".XML";
            struct stat st;
            if (stat((mpc::contentPath("SYNTHS") + "/" + relative).c_str(), &st) == 0) continue;
            EngineHold engine(engineInstance_.load());
            // Anything process() has not yet taken belongs in the file too.
            for (int m = 0; m < kNumEngineParams; ++m)
            {
                const int value = pending_[m].exchange(kNoChange);
                if (value != kNoChange) engineSet(m, value);
            }
            applyCableSlots();
            error = deluge_hosted::engineSaveSynth(("SYNTHS/" + relative).c_str());
            break;
        }
        const int next = (saveTextNow_.load() + 1) & 1;
        const size_t slash = relative.rfind('/');
        if (error == 0) std::snprintf(saveText_[next], sizeof saveText_[next], "%s", relative.substr(slash == std::string::npos ? 0 : slash + 1, std::string::npos).c_str());
        else std::snprintf(saveText_[next], sizeof saveText_[next], "not saved (error %d)", error);
        if (error == 0) { char* dot = std::strrchr(saveText_[next], '.'); if (dot != nullptr) *dot = 0; }
        saveTextNow_.store(next);
        saves_.fetch_add(1);
        saveError_.store(error);
        // The parameter's number never changes (it would then differ from what a project holds);
        // telling the MPC it has is what makes it ask for the text again.
        tellHostLater(kSaveResult);
    }

    // --- the oscillators' sample files ---------------------------------------------------------
    // Like Preset and Folder: settled here and now, on whatever thread the MPC calls from; the
    // worker thread loads the file (applySamples) and tells the MPC what moved.
    void sampleParameterChanged(int index, float plainValue)
    {
        const int s = (index - kSample) / kSParams, which = (index - kSample) % kSParams;
        const bool pressed = plainValue > 0.5f;
        switch (which)
        {
            case kSFile:
                if (tMirroringKnobs) break;   // the worker showing what the engine holds
                wantedSample_[s].store(mpc::clamp((int)std::lround(plainValue), 0, (int)sampleFiles_.size()));
                followSampleFolder(s);
                break;
            case kSFolder:     if (!tSettingFolder && !tMirroringKnobs) browseSample(s, 0, 0, (int)std::lround(plainValue)); break;
            case kSFolderPrev: buttonSet(index, plainValue); if (pressed) browseSample(s, 0, -1, -1); break;
            case kSFolderNext: buttonSet(index, plainValue); if (pressed) browseSample(s, 0, +1, -1); break;
            case kSFilePrev:   buttonSet(index, plainValue); if (pressed) browseSample(s, -1, 0, -1); break;
            case kSFileNext:   buttonSet(index, plainValue); if (pressed) browseSample(s, +1, 0, -1); break;
        }
    }
    int sampleFolderOf(int file) const   // file: index into sampleFiles_
    {
        int f = 0;
        for (size_t i = 0; i < sampleFolderStart_.size(); ++i) if (sampleFolderStart_[i] <= file) f = (int)i;
        return f;
    }
    void browseSample(int s, int fileSteps, int folderSteps, int folderSet)
    {
        if (sampleFiles_.empty()) return;
        const int last = (int)sampleFiles_.size() - 1, lastFolder = (int)sampleFolders_.size() - 1;
        const int before = getInt(kSample + s * kSParams + kSFile) - 1;   // -1: none
        int file = before;
        if (fileSteps != 0) file = mpc::clamp(file + fileSteps, 0, last);
        if (folderSteps != 0) file = sampleFolderStart_[(size_t)mpc::clamp(sampleFolderOf(std::max(file, 0)) + folderSteps, 0, lastFolder)];
        if (folderSet >= 0 && (file < 0 || mpc::clamp(folderSet, 0, lastFolder) != sampleFolderOf(file)))
            file = sampleFolderStart_[(size_t)mpc::clamp(folderSet, 0, lastFolder)];
        if (file != before) set(kSample + s * kSParams + kSFile, (float)(file + 1));   // back through sampleParameterChanged
        else followSampleFolder(s);
        sampleBrowseChanged_.store(true);
    }
    void followSampleFolder(int s)
    {
        const int file = getInt(kSample + s * kSParams + kSFile) - 1;
        if (file < 0 || sampleFiles_.empty()) return;
        const float folder = (float)sampleFolderOf(file);
        if (get(kSample + s * kSParams + kSFolder) != folder)
        {
            tSettingFolder = true;
            set(kSample + s * kSParams + kSFolder, folder);
            tSettingFolder = false;
        }
        sampleBrowseChanged_.store(true);
    }
    // Which of sampleFiles_ the engine's oscillator plays, or -1. Engine lock held.
    int engineSampleIndex(int s) const { return sampleIndexOfId(deluge_hosted::engineSampleIdOf(s)); }
    int sampleIndexOfId(uint32_t id) const
    {
        if (id == 0) return -1;
        for (size_t i = 0; i < sampleIds_.size(); ++i) if (sampleIds_[i] == id) return (int)i;
        return -1;
    }
    // Worker thread: the File and Folder parameters show the engine's files, read now (engine
    // lock held) or as the audio thread last saw them.
    void showSamples(bool fromSnapshot = false)
    {
        for (int s = 0; s < 2; ++s)
        {
            if (fromSnapshot && wantedSample_[s].load() != kNoChange) continue;   // a choice not yet loaded
            const int file = fromSnapshot ? sampleIndexOfId(snapSample_[s].load()) : engineSampleIndex(s);
            const int index = kSample + s * kSParams + kSFile;
            if (getInt(index) != file + 1)
            {
                tMirroringKnobs = true;
                set(index, (float)(file + 1));
                tMirroringKnobs = false;
                tellHostLater(index);
            }
            followSampleFolder(s);
        }
    }
    // Worker thread, engine lock held: gives the engine the file a File parameter asks for.
    // Returns whether the engine changed.
    bool giveSample(int s, int value)
    {
        if (value <= 0 || value > (int)sampleFiles_.size())
        {
            // "None of these": the preset's own file, if the engine has been given another since.
            // A preset with no file there has its zone emptied; its oscillator type comes back
            // from the Type pad.
            if (value != 0 || !sampleChosen_[s].exchange(false)) return false;
            const uint32_t now = deluge_hosted::engineSampleIdOf(s);
            if (presetSample_[s][0] == 0 ? now == 0 : now == deluge_hosted::engineSampleId(presetSample_[s])) return false;
            sampleError_.store(deluge_hosted::engineSampleSet(s, presetSample_[s]));
            return true;
        }
        if (engineSampleIndex(s) == value - 1) return false;
        const int error = deluge_hosted::engineSampleSet(s, sampleFiles_[(size_t)value - 1].c_str());
        sampleError_.store(error);
        if (error == -1) return false;   // not in this synth mode: nothing changed
        sampleChosen_[s].store(true);
        sampleLoads_.fetch_add(1);
        int samplesLoaded = 0, samplesFailed = 0;
        deluge_hosted::engineSampleCounts(&samplesLoaded, &samplesFailed);
        samplesLoaded_.store(samplesLoaded);
        samplesFailed_.store(samplesFailed);
        noteSamples(loadedPreset_.load());
        return true;
    }
    // Worker thread: a file chosen on the MPC since the preset settled.
    void applySamples()
    {
        if (wantedSample_[0].load() == kNoChange && wantedSample_[1].load() == kNoChange) return;
        const int loaded = loadedPreset_.load();
        if (loaded != requestedPreset_.load() || goldPreset_.load() != loaded || resync_.load() || reloadPreset_.load()) return;   // syncKnobs() will
        samplesBusy_.store(true);   // before the requests are taken: waitUntilSettled() looks at both
        struct Done { std::atomic<bool>& busy; ~Done() { busy.store(false); } } done{samplesBusy_};
        EngineHold engine(engineInstance_.load());
        bool changed = false;
        for (int s = 0; s < 2; ++s)
        {
            const int wanted = wantedSample_[s].exchange(kNoChange);
            if (wanted != kNoChange) changed = giveSample(s, wanted) || changed;
        }
        showSamples();
        if (changed) showEngineGrid();   // a new file brings the sample pads to life and may move the play mode
        snapSeen_ = snapSerial_.load();  // an earlier read-back is of the engine before this, with the old file
        snapSeen_ = snapSerial_.load();  // an earlier read-back is of the engine before this, with the old file
    }
    // Worker thread, while a project save or load waits for the settled state: values set from
    // outside that process() has not yet given the engine (it does so at the start of a block,
    // and audio may not be running) go to it now, and the parameters show what the engine then
    // holds (a read-back the audio thread has taken is not shown while someone waits). Once for
    // each wait. Otherwise what is saved depends on whether a block happened to run, and a
    // read-back to be shown, before the save.
    void drainForWaiter()
    {
        const uint32_t asked = settleAsked_.load();
        if (waiters_.load() == 0 || settleDone_.load() == asked) return;
        const int loaded = loadedPreset_.load();
        if (loaded != requestedPreset_.load() || goldPreset_.load() != loaded || resync_.load() || reloadPreset_.load()) return;   // syncKnobs() will
        samplesBusy_.store(true);
        struct Done { std::atomic<bool>& busy; ~Done() { busy.store(false); } } done{samplesBusy_};
        EngineHold engine(engineInstance_.load());
        anyPending_.store(false);
        for (int m = 0; m < kNumEngineParams; ++m)
        {
            const int value = pending_[m].exchange(kNoChange);
            if (value != kNoChange) engineSet(m, value);
        }
        applyCableSlots();
        for (int k = 0; k < deluge_hosted::kNumKnobs; ++k)
        {
            int position = 0;
            if (deluge_hosted::engineKnobGet(k, &position)) showEngineValue(k, position, lastHostChange_[k].load());
        }
        showEngineGrid();
        showSamples();
        reconcileCables(false);
        showCables(changeCounter_.load());
        settleDone_.store(asked);
    }

    // Worker thread, engine lock held: every pad shows what the engine holds, unless the MPC has
    // set it since.
    void showEngineGrid()
    {
        int32_t values[deluge_hosted::kGridSlots];
        uint8_t relevant[deluge_hosted::kGridSlots];
        deluge_hosted::engineGridRead(values, relevant);
        for (int c = 0; c < grid::kNumCells; ++c)
        {
            const int slot = slotOf(grid::kCells[c]);
            const bool nowRelevant = relevant[slot] != 0;
            if (cellRelevant_[c].exchange(nowRelevant) != nowRelevant) tellHostOf(kFirstCell + c);
            if (!nowRelevant || pending_[kFirstCell + c].load() != kNoChange) continue;
            showEngineValue(kFirstCell + c, (int)values[slot], lastHostChange_[kFirstCell + c].load());
        }
        snapSeen_ = snapSerial_.load();
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
        if (preset != before) selectPreset(preset);
        browseChanged_.store(true);
    }

    // Makes Folder and Preset name this preset (a position in presets_) and asks for it.
    void selectPreset(int preset)
    {
        if (presets_.empty()) return;
        preset = mpc::clamp(preset, 0, (int)presets_.size() - 1);
        const int folder = folderIndexOfPreset(preset);
        tSettingFolder = true;
        tSettingPreset = true;
        if (getInt(kFolder) != folder) set(kFolder, (float)folder);
        if (getInt(kPreset) != preset - folderStart_[(size_t)folder]) set(kPreset, (float)(preset - folderStart_[(size_t)folder]));
        tSettingFolder = false;
        tSettingPreset = false;
        requestedPreset_.store(preset);
        browseChanged_.store(true);
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index == kPreset)
        {
            if (presets_.empty()) { out = "(none)"; return true; }
            const int folder = mpc::clamp(getInt(kFolder), 0, (int)folders_.size() - 1);
            const std::string& path = presets_[(size_t)(folderStart_[(size_t)folder] + mpc::clamp((int)std::lround(plainValue), 0, folderSize(folder) - 1))];
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
        if (index >= kCable && index < kPresetBack10)
        {
            const int which = (index - kCable) % 4, v = (int)std::lround(plainValue);
            char text[16];
            if (which == kCAmount) { std::snprintf(text, sizeof text, "%.2f", (double)v / 100.0); out = text; }
            else if (which == kCDestination) out = v >= 1 && v <= cables::kNumDestinations ? cables::kDestinations[v - 1] : "-";
            else out = v >= 1 && v <= cables::kNumSources ? cables::kSources[v - 1] : "-";
            return true;
        }
        if (index == kSaveResult)
        {
            out = saveText_[saveTextNow_.load() & 1];
            return true;
        }
        if (index == kSampleState)
        {
            out = sampleText_[sampleTextNow_.load() & 1];
            return true;
        }
        if (index >= kSample && index < kSave)
        {
            const int s = (index - kSample) / kSParams, which = (index - kSample) % kSParams;
            const int v = (int)std::lround(plainValue);
            if (which == kSFile)
            {
                // The file's name without its folder or ".wav". 0 is "no file from SAMPLES".
                if (v <= 0 || v > (int)sampleFiles_.size()) { out = "-"; return true; }
                const std::string& path = sampleFiles_[(size_t)v - 1];
                const size_t start = path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1;
                const size_t dot = path.rfind('.');
                out = path.substr(start, dot != std::string::npos && dot > start ? dot - start : std::string::npos);
                return true;
            }
            if (which == kSFolder)
            {
                if (sampleFolders_.empty()) { out = "(none)"; return true; }
                const std::string& f = sampleFolders_[(size_t)mpc::clamp(v, 0, (int)sampleFolders_.size() - 1)];
                out = f.size() > 8 ? f.substr(8) : "(top)";   // without "SAMPLES/"
                return true;
            }
            (void)s;
            return false;
        }
        if (index >= kFine && index < kSample)
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
        // First let whatever is on its way in arrive (a preset, a sample, the knob hand-over
        // that follows): what is written below, and what the MPC looks at next, is then the
        // settled state.
        waitUntilSettled();
        const int i = presetIndex();
        const bool goldIsAnotherPresets = goldPreset_.load() != i;
        if (i >= 0)
        {
            out.assign(presets_[(size_t)i].begin(), presets_[(size_t)i].end());
            out.push_back(0);
            if (goldIsAnotherPresets) out.push_back('K');
        }
        // The MPC wrote the parameters into the project before calling this, so before that
        // wait: a sample still loading, or pads the engine had not yet been read back for, are
        // in there as they stood. The settled values go here and loadExtraState() puts them
        // over the MPC's. After the flags: a zero byte, "V", how many, each as a float.
        if (i >= 0 && !goldIsAnotherPresets)
        {
            out.push_back(0);
            out.push_back('V');
            const uint16_t count = (uint16_t)(kNumParams - kGold);
            out.insert(out.end(), reinterpret_cast<const uint8_t*>(&count), reinterpret_cast<const uint8_t*>(&count) + sizeof count);
            for (int index = kGold; index < kNumParams; ++index)
            {
                // A pad that does not apply to the sound as it stands (an oscillator's type
                // while the synth mode is FM) holds a parameter value that means nothing: the
                // engine never took it. Not-a-number says so, and a restore leaves that pad as
                // the preset has it rather than give the engine the stale value the moment the
                // pad comes to life.
                const int cell = index >= kFine && index < kSample ? cellOfFine_[index - kFine] : (index >= kGrid && index < kFine ? index - kGrid : -1);
                const float value = cell >= 0 && !cellRelevant_[cell].load() ? std::nanf("") : get(index);
                out.insert(out.end(), reinterpret_cast<const uint8_t*>(&value), reinterpret_cast<const uint8_t*>(&value) + sizeof value);
            }
            // The oscillators' sample files by path ("S", then each path and a zero byte): a File
            // parameter is a position in the sorted list of files, which moves when files are
            // added to Content/SAMPLES.
            out.push_back('S');
            for (int osc = 0; osc < 2; ++osc)
            {
                const int file = getInt(kSample + osc * kSParams + kSFile);
                if (file >= 1 && file <= (int)sampleFiles_.size()) out.insert(out.end(), sampleFiles_[(size_t)file - 1].begin(), sampleFiles_[(size_t)file - 1].end());
                out.push_back(0);
            }

        }
        writeDiagnostics();   // a project save is a convenient moment to refresh the file
    }

    // Runs on the MPC's thread, straight after it has set every parameter from the project.
    void loadExtraState(const uint8_t* data, size_t size) override
    {
        saveRequested_.store(false);   // a Save pressed just before belongs to the state being replaced
        const uint8_t* end = static_cast<const uint8_t*>(std::memchr(data, 0, size));
        const size_t pathSize = end != nullptr ? (size_t)(end - data) : size;
        // The flags run from the first zero byte to the next; the settled values follow.
        const uint8_t* flags = end != nullptr ? end + 1 : data + size;
        const uint8_t* flagsEnd = static_cast<const uint8_t*>(std::memchr(flags, 0, (size_t)(data + size - flags)));
        if (flagsEnd == nullptr) flagsEnd = data + size;
        const bool goldIsThisPresets = std::memchr(flags, 'K', (size_t)(flagsEnd - flags)) == nullptr;
        const uint8_t* values = nullptr;
        if (data + size - flagsEnd >= 4 && flagsEnd[1] == 'V')
        {
            uint16_t count = 0;
            std::memcpy(&count, flagsEnd + 2, sizeof count);
            if (count == kNumParams - kGold && (size_t)(data + size - (flagsEnd + 4)) >= count * sizeof(float)) values = flagsEnd + 4;
        }
        // After the values, the sample files by path (see saveExtraState).
        std::string samplePaths[2];
        bool haveSamplePaths = false;
        if (values != nullptr)
        {
            const uint8_t* at = values + (size_t)(kNumParams - kGold) * sizeof(float);
            const uint8_t* const stop = data + size;
            if (at < stop && *at == 'S')
            {
                ++at;
                haveSamplePaths = true;
                for (int osc = 0; osc < 2 && at < stop; ++osc)
                {
                    const uint8_t* zero = static_cast<const uint8_t*>(std::memchr(at, 0, (size_t)(stop - at)));
                    if (zero == nullptr) zero = stop;
                    samplePaths[osc].assign(reinterpret_cast<const char*>(at), (size_t)(zero - at));
                    at = zero < stop ? zero + 1 : stop;
                }
            }
        }
        const std::string wanted(reinterpret_cast<const char*>(data), pathSize);
        statRestores_.fetch_add(1);
        if (!goldIsThisPresets) statRestoresWithoutKnobs_.fetch_add(1);
        for (size_t i = 0; i < presets_.size(); ++i)
        {
            if (presets_[i] == wanted) { selectPreset((int)i); break; }
        }
        // (Not found: the file was removed or renamed. Keep whatever the Preset number selects.)

        if (goldIsThisPresets && values != nullptr)
        {
            // The settled values (saveExtraState) are the state to come back to, whatever the
            // MPC has just set and whatever the engine is in the middle of. The worker thread
            // takes them when this preset has been loaded afresh (syncKnobs), which makes the
            // restore one step from a known start: the preset as it is on the card, then these.
            {
                std::lock_guard<std::mutex> lock(restoreLock_);
                restoreValues_.resize((size_t)(kNumParams - kGold));
                std::memcpy(restoreValues_.data(), values, restoreValues_.size() * sizeof(float));
                // A file saved by path takes its place in today's list (or none, if it is no
                // longer there: better silent than some other file).
                for (int osc = 0; osc < 2 && haveSamplePaths; ++osc)
                {
                    int file = 0;
                    for (size_t i = 0; i < sampleFiles_.size() && file == 0; ++i)
                        if (strcasecmp(sampleFiles_[i].c_str(), samplePaths[osc].c_str()) == 0) file = (int)i + 1;
                    restoreValues_[(size_t)(kSample + osc * kSParams + kSFile - kGold)] = (float)file;
                    // Its Folder parameter follows from the file; the saved number may be stale.
                    // (With no file, Folder is only where the user was looking: as saved.)
                    if (file != 0) restoreValues_[(size_t)(kSample + osc * kSParams + kSFolder - kGold)] = std::nanf("");
                }
            }
            restorePending_.store(true);
            reloadPreset_.store(true);
            waitUntilSettled();
            return;
        }
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

        // The host's tempo, for synced delays, synced LFOs and the arpeggiator. Every block: it is
        // one comparison when nothing has changed, and a preset load keeps it.
        if (ctx.transport.valid && ctx.transport.tempoValid)
        {
            deluge_hosted::engineSetTempo((float)ctx.transport.bpm);
            statTempoGiven_.store((float)ctx.transport.bpm, std::memory_order_relaxed);
        }
        statTempo_.store(deluge_hosted::engineTempo(), std::memory_order_relaxed);

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
        if (cableDirty_.load(std::memory_order_relaxed) != 0 && applyCableSlots()) snapshotWanted_ = true;
        framesSinceSnapshot_ = std::min(framesSinceSnapshot_ + numFrames, kSnapshotSpacing);
        if (snapshotWanted_ && framesSinceSnapshot_ >= kSnapshotSpacing)
        {
            snapshotWanted_ = false;
            framesSinceSnapshot_ = 0;
            takeSnapshot(changesBefore);
        }
        const float targetGain = dbToGain(get(kVolume));

        // Sidechain steps. A Deluge's sidechain is hit by a kit row playing in the same song;
        // here each instance is a song with one synth in it, so the hits come from a pattern of
        // sixteen steps laid over the MPC's bar. Where in this block does a sixteenth begin?
        // The position the MPC gives is the block's first frame, in quarter notes.
        int hitFrames[8];
        int numHits = 0;
        statPpq_.store(ctx.transport.valid && ctx.transport.ppqValid ? (float)ctx.transport.ppqPos : -1.0f, std::memory_order_relaxed);
        if (ctx.transport.valid && ctx.transport.ppqValid && ctx.transport.playing && ctx.transport.bpm > 0.0)
        {
            const double sixteenth = (ctx.transport.ppqPos - ctx.transport.barStartPpq) * 4.0;   // since the bar began
            const double framesPerSixteenth = ctx.sampleRate * 60.0 / ctx.transport.bpm / 4.0;
            double next = std::ceil(sixteenth - 1e-9);
            // A position that has not moved on from the last block's (some hosts repeat it while
            // looping back) must not hit the same step twice.
            if (ctx.transport.ppqPos == lastHitPpq_) next += 1.0;
            for (; numHits < 8; next += 1.0)
            {
                // Rounded down, so that a step a fraction of a frame before the block ends falls in
                // this block: the next block starts past it and would not see it.
                const int at = (int)std::floor((next - sixteenth) * framesPerSixteenth);
                if (at >= numFrames) break;
                const int step = (int)((long long)next % 16 + 16) % 16;
                if (get(kScStep + step) > 0.5f)
                {
                    hitFrames[numHits++] = std::max(0, at);
                    if (at <= 0) lastHitPpq_ = ctx.transport.ppqPos;
                }
            }
        }
        if (numHits != 0) statScHits_.fetch_add((uint32_t)numHits, std::memory_order_relaxed);

        int frame = 0;
        int midiIndex = 0;
        int hitIndex = 0;
        while (frame < numFrames)
        {
            while (hitIndex < numHits && hitFrames[hitIndex] <= frame)
            {
                deluge_hosted::engineSideChainHit();
                ++hitIndex;
            }
            while (midiIndex < ctx.numMidiEvents && ctx.midi[midiIndex].sampleOffset <= frame)
            {
                handleMidi(ctx.midi[midiIndex]);
                ++midiIndex;
            }
            int segmentEnd = numFrames;
            if (midiIndex < ctx.numMidiEvents)
                segmentEnd = mpc::clamp(ctx.midi[midiIndex].sampleOffset, frame + 1, numFrames);
            if (hitIndex < numHits) segmentEnd = std::min(segmentEnd, std::max(hitFrames[hitIndex], frame + 1));

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
        for (int s = 0; s < 2; ++s) snapSample_[s].store(deluge_hosted::engineSampleIdOf(s), std::memory_order_relaxed);
        reconcileCables(false);
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

    // Sample files under Content/SAMPLES (WAV and AIFF, which is what the firmware reads), as the
    // engine names them: relative to Content/.
    void scanSamples()
    {
        std::vector<std::string> found;
        presets_.swap(found);   // scanFolder() fills presets_
        scanFolder(mpc::contentPath("SAMPLES"), "SAMPLES", 0, true);
        presets_.swap(found);
        std::sort(found.begin(), found.end(), lessIgnoringCase);
        if (found.size() > (size_t)kMaxPresets) found.resize((size_t)kMaxPresets);
        sampleFiles_ = found;
        for (const std::string& file : sampleFiles_) sampleIds_.push_back(deluge_hosted::engineSampleId(file.c_str()));
        for (size_t i = 0; i < sampleFiles_.size(); ++i)
        {
            const std::string folder = folderOf(sampleFiles_[i]);
            if (sampleFolders_.empty() || sampleFolders_.back() != folder) { sampleFolders_.push_back(folder); sampleFolderStart_.push_back((int)i); }
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

    void scanFolder(const std::string& absolute, const std::string& relative, int depth, bool samples = false)
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
            const char* ext = name.rfind('.') != std::string::npos ? name.c_str() + name.rfind('.') : "";
            if (S_ISDIR(st.st_mode)) { if (depth < 6) scanFolder(childAbsolute, childRelative, depth + 1, samples); }
            else if (samples ? (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".aif") == 0 || strcasecmp(ext, ".aiff") == 0)
                             : strcasecmp(ext, ".xml") == 0)
                presets_.push_back(childRelative);
        }
        closedir(d);
    }

    // The selected preset as a position in presets_: the folder Folder names, and in it the
    // position Preset names (past the folder's end: its last).
    int presetIndex() const
    {
        if (presets_.empty()) return -1;
        const int folder = mpc::clamp(getInt(kFolder), 0, (int)folders_.size() - 1);
        return folderStart_[(size_t)folder] + mpc::clamp(getInt(kPreset), 0, folderSize(folder) - 1);
    }
    int folderSize(int folder) const
    {
        const int end = folder + 1 < (int)folderStart_.size() ? folderStart_[(size_t)folder + 1] : (int)presets_.size();
        return std::max(1, end - folderStart_[(size_t)folder]);
    }

    // Loads the requested preset whenever it differs from the loaded one. Holding the engine lock
    // makes process() output silence for the duration.
    void workerLoop()
    {
        while (!quit_.load())
        {
            applyBrowsing();
            loadRequestedPreset();
            applySamples();
            applySave();
            drainForWaiter();
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
        // A Preset turned past the end of its folder comes back to the last one, so that turning
        // it down again takes effect at once.
        if (!presets_.empty())
        {
            const int last = folderSize(mpc::clamp(getInt(kFolder), 0, (int)folders_.size() - 1)) - 1;
            if (getInt(kPreset) > last && nowMs() - presetTurnedMs_.load() > 400)
            {
                tSettingPreset = true;
                set(kPreset, (float)last);
                tSettingPreset = false;
                changed = true;
            }
        }
        if (changed) for (int index : {kPreset, kFolder}) tellHostLater(index);
        const int64_t now = nowMs();
        if (sampleBrowseChanged_.exchange(false))
            for (int s = 0; s < 2; ++s) for (int which : {kSFolder, kSFile}) tellHostLater(kSample + s * kSParams + which);
        for (int slot = 0; slot < kNumButtons; ++slot)
        {
            const int button = buttonIndex(slot);
            if (get(button) == 0.0f || now - buttonOnSince_[slot].load() < 300) continue;   // long enough for the slow emulator to read it back
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
        showSamples(true);   // a pad can take an oscillator's file away (its type changed from Sample)
        showCables(changes);
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
        settleAsked_.fetch_add(1);
        waiters_.fetch_add(1);
        bool done = false;
        for (int ms = 0; ms < 3000; ++ms)
        {
            const int requested = requestedPreset_.load();
            const bool settled = loadedPreset_.load() == requested && goldPreset_.load() == requested
                                 && !reloadPreset_.load() && !resync_.load() && !knobsBusy_.load()
                                 && wantedSample_[0].load() == kNoChange && wantedSample_[1].load() == kNoChange
                                 && !samplesBusy_.load() && settleDone_.load() == settleAsked_.load()
                                 && !restorePending_.load();
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
        // Which files are the preset's own, to go back to (giveSample).
        for (int s = 0; s < 2; ++s)
        {
            sampleChosen_[s].store(false);
            deluge_hosted::engineSampleGet(s, presetSample_[s], (int)sizeof presetSample_[s]);
        }
        int samplesLoaded = 0, samplesFailed = 0;
        deluge_hosted::engineSampleCounts(&samplesLoaded, &samplesFailed);
        samplesLoaded_.store(samplesLoaded);
        samplesFailed_.store(samplesFailed);
        noteSamples(wanted);
        loadedPreset_.store(wanted);
        loads_.fetch_add(1);
        syncKnobs();
        noteFirmwareErrors();
    }

    // Worker thread, engine lock held, after a preset or a sample file has been loaded: say on
    // MAIN how the sound's sample files fared, and add the ones found nowhere to
    // missing-samples.txt in the plugin folder, each line once, so that someone whose preset is
    // silent can see which files to copy and where to.
    void noteSamples(int loaded)
    {
        const int elsewhere = deluge_hosted::engineSamplesFoundElsewhere();
        std::vector<std::string> missing;
        char path[512];
        for (int i = 0; deluge_hosted::engineMissingSample(i, path, (int)sizeof path); ++i) missing.push_back(path);
        samplesElsewhere_.store(elsewhere);
        samplesMissing_.store((int)missing.size());
        const int next = (sampleTextNow_.load() + 1) & 1;
        const int used = samplesLoaded_.load() + (int)missing.size();
        if (!missing.empty()) std::snprintf(sampleText_[next], sizeof sampleText_[next], "%d of %d MISSING", (int)missing.size(), used);
        else if (used == 0) std::snprintf(sampleText_[next], sizeof sampleText_[next], "none used");
        else if (elsewhere > 0) std::snprintf(sampleText_[next], sizeof sampleText_[next], "%d found, %d by name", used, elsewhere);
        else std::snprintf(sampleText_[next], sizeof sampleText_[next], "%d found", used);
        sampleTextNow_.store(next);
        tellHostLater(kSampleState);
        if (missing.empty()) return;

        const std::string file = mpc::pluginFolder() + "/missing-samples.txt";
        if (!missingRead_)
        {
            missingRead_ = true;
            if (FILE* f = std::fopen(file.c_str(), "r"))
            {
                char line[1200];
                while (std::fgets(line, sizeof line, f) != nullptr)
                {
                    line[std::strcspn(line, "\r\n")] = 0;
                    missingWritten_.insert(line);
                }
                std::fclose(f);
            }
        }
        const std::string preset = loaded >= 0 && loaded < (int)presets_.size() ? presets_[(size_t)loaded] : "?";
        FILE* f = nullptr;
        for (const std::string& name : missing)
        {
            const std::string line = "Content/" + name + "    wanted by SYNTHS/" + preset;
            if (!missingWritten_.insert(line).second) continue;
            if (f == nullptr)
            {
                const bool fresh = missingWritten_.size() == 1;
                f = std::fopen(file.c_str(), "a");
                if (f == nullptr) return;
                if (fresh)
                    std::fputs("Sample files that presets asked for and Deluge Synth could not find. Each line is where the\n"
                               "file should be, inside the plugin folder; a file with that name anywhere under Content/SAMPLES\n"
                               "is found too. Delete this file to start the list again.\n", f);
            }
            std::fprintf(f, "%s\n", line.c_str());
        }
        if (f != nullptr) std::fclose(f);
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
        // A project restore waiting for this preset (loadExtraState): its values become the
        // parameters now, as if set from outside, and are kept.
        if (restorePending_.load() && loaded == requestedPreset_.load())
        {
            std::vector<float> values;
            {
                std::lock_guard<std::mutex> lock(restoreLock_);
                values = restoreValues_;
            }
            for (int index = kGold; index < kNumParams && (size_t)(index - kGold) < values.size(); ++index)
            {
                if (index == kSave || index == kSaveResult || index >= kPresetBack10) continue;
                if (index >= kSample && index < kSave && (index - kSample) % kSParams >= kSFolderPrev) continue;   // the buttons
                const float value = values[(size_t)(index - kGold)];
                if (std::isnan(value))
                {
                    // Did not apply when saved (see saveExtraState): the rounds below pass it by.
                    const int cell = index >= kFine && index < kSample ? cellOfFine_[index - kFine] : (index >= kGrid && index < kFine ? index - kGrid : -1);
                    if (cell >= 0) leaveCell_[cell] = true;
                    continue;
                }
                if (get(index) != value) { set(index, value); tellHostLater(index); }
            }
            goldPreset_.store(loaded);
            restorePending_.store(false);
        }
        const bool keep = goldPreset_.load() == loaded;
        const bool adopt = !keep && loaded == requestedPreset_.load();
        const int changesBefore = knobChanges_.load();
        knobsBusy_.store(true);
        // The cables first: gold knobs and pads that reach a cable's depth come after and agree.
        if (keep) giveCables();
        else if (adopt) cableDirty_.store(0);   // a slot changed for the last preset
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
        // A sample file asked for but not yet given belonged to the last state: forgotten. Kept
        // File parameters go to the engine at the start of each round below, before the pads
        // (the sample pads only apply once an oscillator has its file), and in every round
        // because the engine takes a file only in subtractive mode, which a pad may have to set
        // first.
        for (int s = 0; s < 2; ++s) wantedSample_[s].store(kNoChange);
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
                bool fileChanged = false;
                for (int s = 0; s < 2; ++s) fileChanged = giveSample(s, getInt(kSample + s * kSParams + kSFile)) || fileChanged;
                if (fileChanged) deluge_hosted::engineGridRead(before, relevantBefore);
                for (int c = 0; c < grid::kNumCells; ++c)
                {
                    const grid::Cell& cell = grid::kCells[c];
                    const int slot = slotOf(cell);
                    if (!relevantBefore[slot] || leaveCell_[c] || before[slot] == shownValue(kFirstCell + c)) continue;
                    deluge_hosted::engineCellSet(cell.layer, cell.x, cell.y, shownValue(kFirstCell + c));
                }
                deluge_hosted::engineGridRead(values, relevant);
                bool changed = false;
                for (int c = 0; c < grid::kNumCells && !changed; ++c)
                {
                    const int slot = slotOf(grid::kCells[c]);
                    changed = relevant[slot] != relevantBefore[slot] || (relevant[slot] && values[slot] != before[slot]);
                }
                if (!changed && !fileChanged) break;
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
        if (keep || adopt) showSamples();
        if (keep || adopt)
        {
            reconcileCables(adopt);
            showCables(changeCounter_.load());
        }
        for (bool& leave : leaveCell_) leave = false;

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
        char text[2400];
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
                      "sample files found in Content/SAMPLES: %d in %d folders; files chosen for an oscillator %u, last error %d\n"
                      "mod matrix: %d cable slots, %d cables beyond them (they play, unseen)\n"
                      "saved as a Deluge preset %u times, last as \"%s\", error %d\n"
                      "firmware error stops (its own consistency checks; should be 0): %d%s%s\n"
                      "tempo: the host last gave %.2f (0: never), the engine is at %.2f\n"
                      "sidechain steps: %u hits given; the host's position was last %.2f quarter notes (-1: it gives none)\n"
                      "this preset's sample files: %d found by name in another folder, %d found nowhere (see missing-samples.txt)\n",
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
                      (int)sampleFiles_.size(), (int)sampleFolders_.size(), sampleLoads_.load(), sampleError_.load(),
                      kCableSlots, cablesHidden_.load(),
                      saves_.load(), saveText_[saveTextNow_.load() & 1], saveError_.load(),
                      firmwareErrors_.load(), firmwareErrors_.load() != 0 ? ", latest " : "", latestFirmwareError_,
                      (double)statTempoGiven_.load(), (double)statTempo_.load(),
                      statScHits_.load(), (double)statPpq_.load(),
                      samplesElsewhere_.load(), samplesMissing_.load());
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
    int               cellOfFine_[kNumFine] = {};
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
    static inline thread_local bool tSettingPreset = false;
    std::atomic<int64_t> presetTurnedMs_{0};   // when Preset was last set from outside
    static inline thread_local bool tReleasingButton = false;
    std::atomic<int64_t> buttonOnSince_[kNumButtons];   // when each button last went on, in ms
    std::vector<std::string> sampleFiles_;        // under Content/, "SAMPLES/Kicks/909.wav", sorted; constructor only
    std::vector<std::string> sampleFolders_;      // the folders they are in, and where each starts
    std::vector<int>         sampleFolderStart_;
    std::vector<uint32_t>    sampleIds_;          // engineSampleId of each
    std::atomic<uint32_t> snapSample_[2] = {};    // each oscillator's file at the last read-back
    std::atomic<int>  wantedSample_[2];           // a File parameter set from outside, not yet given to the engine
    std::atomic<bool> sampleBrowseChanged_{false};
    std::atomic<uint32_t> settleAsked_{0}, settleDone_{0};   // waits begun, and the last one the worker read the engine back for
    bool                  leaveCell_[grid::kNumCells] = {};   // worker thread: pads a restore does not set
    CableSlot             slots_[kCableSlots];                 // engine lock held: what each slot is taken to hold
    std::atomic<int>      cableHost_[kCableSlots][4];          // the slot parameters as last set from outside, or shown
    std::atomic<int>      cableStamp_[kCableSlots];            // changeCounter_ when a slot was last set from outside
    std::atomic<uint32_t> cableDirty_{0};                      // slots set from outside, not yet given to the engine
    std::atomic<int>      snapSlot_[kCableSlots][4];           // slots_ at the last reconcile, for the worker
    std::atomic<int>      cablesHidden_{0};                    // cables the engine has beyond the slots
    std::atomic<bool>     saveRequested_{false};
    char                  sampleText_[2][48] = {"none used", "none used"};   // likewise, for Samples
    std::atomic<int>      sampleTextNow_{0};
    std::atomic<int>      samplesElsewhere_{0}, samplesMissing_{0};
    bool                  missingRead_ = false;        // worker thread only, with the set below
    std::set<std::string> missingWritten_;             // the lines missing-samples.txt holds
    char                  saveText_[2][80] = {};  // the worker writes the one not showing, then switches
    std::atomic<int>      saveTextNow_{0};
    std::atomic<uint32_t> saves_{0};
    std::atomic<int>      saveError_{0};
    std::mutex            restoreLock_;           // loadExtraState and the worker; never the audio thread
    std::vector<float>    restoreValues_;         // a restored project's parameter values, from kGold on
    std::atomic<bool>     restorePending_{false}; // not yet taken by the worker
    std::atomic<bool> samplesBusy_{false};        // the worker is giving the engine a file
    std::atomic<bool> sampleChosen_[2] = {};      // the engine's file is one chosen here, not the preset's
    char              presetSample_[2][512] = {};  // the loaded preset's own files (worker thread, engine lock held)
    std::atomic<int>  sampleError_{0};
    std::atomic<uint32_t> sampleLoads_{0};
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
    std::atomic<float> statTempoGiven_{0.0f}, statTempo_{0.0f}, statPpq_{-1.0f};
    std::atomic<uint32_t> statScHits_{0};
    double lastHitPpq_ = -1.0;   // process() only
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
