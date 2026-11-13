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
// Choosing a preset: the Preset parameter is the position in the sorted list of preset files, and
// Folder is the folder it sits in. Four button parameters step through presets and folders.
//
// Threads: the engine must never be entered by two threads at once. process() (audio thread)
// takes the engine lock with try_lock and outputs silence if it cannot have it. A worker thread
// owned by the plugin loads presets, holding the lock while it does.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_plugin.h"

#include "engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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
    kNumParams = kGold + deluge_hosted::kNumKnobs
};

// One engine per process (see the header comment).
std::mutex gEngineLock;
std::atomic<void*> gEngineOwner{nullptr};

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
            pendingKnob_[k].store(kNoKnobChange);
            hostKnob_[k].store(0);
            knobNames_[k][0] = 0;
        }
    }

    ~DelugeSynth() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
        workerRunning_.store(false);
        if (gEngineOwner.load() == this)
        {
            std::lock_guard<std::mutex> lock(gEngineLock);
            deluge_hosted::engineAllNotesOff();
            gEngineOwner.store(nullptr);
        }
    }

    // Not the audio thread. The MPC calls this before processing starts and after every
    // suspend/resume.
    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)sampleRate; (void)maxBlockSize;   // the MPC and the Deluge both run at 44100 Hz
        void* nobody = nullptr;
        if (gEngineOwner.load() != this && gEngineOwner.compare_exchange_strong(nobody, this))
        {
            std::lock_guard<std::mutex> lock(gEngineLock);
            deluge_hosted::engineInit(mpc::contentFolder().c_str());
        }
        if (gEngineOwner.load() == this && !worker_.joinable())
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
                // A gold knob moved by the user or the host (not by this thread mirroring the
                // engine): remember it, and process() hands it to the engine.
                if (index >= kGold && index < kGold + deluge_hosted::kNumKnobs && !tMirroringKnobs)
                {
                    const int position = (int)std::lround(plainValue);
                    hostKnob_[index - kGold].store(position);
                    pendingKnob_[index - kGold].store(position);
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
        }
        else
        {
            // Nothing of the user's to keep: load the preset afresh and take its own positions.
            goldPreset_.store(-1);
            reloadPreset_.store(true);
        }
        waitUntilSettled();
    }

    // Makes the gold parameters hold again the values last set from outside (by the MPC).
    void putBackHostKnobs(bool tellHost)
    {
        tMirroringKnobs = true;
        for (int k = 0; k < deluge_hosted::kNumKnobs; ++k)
        {
            const int wanted = hostKnob_[k].load();
            if (getInt(kGold + k) == wanted) continue;
            set(kGold + k, (float)wanted);
            if (tellHost) tellHostLater(kGold + k);
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

        // Silence (the outputs are already zero) when this instance does not own the engine or
        // the worker thread has it (loading a preset, or handing it a restored project's knobs).
        const bool owner = gEngineOwner.load(std::memory_order_relaxed) == this;
        if (!owner || !gEngineLock.try_lock())
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
        for (int k = 0; k < deluge_hosted::kNumKnobs; ++k)
        {
            const int position = pendingKnob_[k].exchange(kNoKnobChange, std::memory_order_relaxed);
            if (position != kNoKnobChange) deluge_hosted::engineKnobSet(k, position);
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
            tellHostNow();
            // Keep the diagnostics file a few seconds fresh while audio runs, so that it says
            // something useful after a crash or a reset too.
            const int64_t now = nowMs();
            if (now - diagnosticsWrittenMs_ >= 3000 && statCalls_.load() != diagnosticsCalls_)
            {
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
        hostToTell_.fetch_or(uint32_t(1) << index);
    }
    void tellHostNow()
    {
        if (waiters_.load() != 0) return;
        const uint32_t which = hostToTell_.exchange(0);
        if (which == 0) return;
        for (int index = 0; index < numParams() && index < 32; ++index)
            if (which & (uint32_t(1) << index)) notifyHostParameterChanged(index);
        requestDisplayUpdate();
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
                                 && !reloadPreset_.load() && !knobsBusy_.load();
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
            // Nothing to load. But the gold parameters may still be another preset's: the user
            // went to a different preset and came back while this one was on its way in.
            if (goldPreset_.load() != wanted)
            {
                std::lock_guard<std::mutex> lock(gEngineLock);
                syncKnobs();
            }
            return;
        }
        const std::string path = "SYNTHS/" + presets_[(size_t)wanted];
        std::lock_guard<std::mutex> lock(gEngineLock);
        loadError_.store(deluge_hosted::engineLoadSynth(path.c_str()));
        int samplesLoaded = 0, samplesFailed = 0;
        deluge_hosted::engineSampleCounts(&samplesLoaded, &samplesFailed);
        samplesLoaded_.store(samplesLoaded);
        samplesFailed_.store(samplesFailed);
        loadedPreset_.store(wanted);
        loads_.fetch_add(1);
        syncKnobs();
    }

    // With a preset in the engine (engine lock held): make the 16 gold parameters and the engine
    // agree. goldPreset_ says which preset the parameter values belong to:
    //
    //   the loaded one     they are the user's (set on the MPC, or restored with a project) and
    //                      go to the engine
    //   another, and the   the user has changed preset: the loaded preset's own knob positions
    //   loaded one is the  become the parameter values, and from now on belong to it
    //   one asked for
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
                pendingKnob_[k].store(kNoKnobChange);
                deluge_hosted::engineKnobSet(k, getInt(kGold + k));
            }
            else if (adopt && deluge_hosted::engineKnobGet(k, &position))
            {
                pendingKnob_[k].store(kNoKnobChange);
                if (getInt(kGold + k) != position) { set(kGold + k, (float)position); tellHostLater(kGold + k); }
                hostKnob_[k].store(position);
            }
        }
        tMirroringKnobs = false;
        if (adopt)
        {
            if (knobChanges_.load() == changesBefore) goldPreset_.store(loaded);
            else
            {
                // A project restore arrived while the loop above was running, and wins: the
                // values it set are in hostKnob_. If they are for this preset they also have to
                // reach the engine; if for another, that preset's load will see to it.
                putBackHostKnobs(true);
                if (goldPreset_.load() == loaded)
                    for (int k = 0; k < deluge_hosted::kNumKnobs; ++k) deluge_hosted::engineKnobSet(k, getInt(kGold + k));
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
    void writeDiagnostics()
    {
        const std::string folder = mpc::pluginFolder();
        if (folder.empty()) return;
        std::lock_guard<std::mutex> lock(diagnosticsLock_);   // the MPC's thread and the worker both write it
        char text[1600];
        const uint64_t frames = statFrames_.load(), ns = statNs_.load();
        const double seconds = (double)frames / 44100.0;
        const int loaded = loadedPreset_.load();
        std::snprintf(text, sizeof text,
                      "Deluge Synth diagnostics (written on prepare, on project/preset save, and every 3 s while audio runs)\n"
                      "engine: %s build; this instance %s the engine\n"
                      "presets found in Content/SYNTHS: %d\n"
                      "loaded preset: %d (%s), load error %d, loads so far %d\n"
                      "its samples: %d held in memory, %d could not be loaded (looked for under Content/, e.g. Content/SAMPLES/...)\n"
                      "process calls %u, of which silent (no engine or loading) %u; note-ons %u; peak out %.4f\n"
                      "time in process(): %.2f s of audio rendered, average %.2f%% of one core, worst block %.1f%%\n"
                      "block sizes asked for: %d to %d frames\n"
                      "samples at the engine's clipping ceiling: %u of %llu (%.3f%%)\n"
                      "project saves %u, restores %u (%u of them without usable knob values); waits that timed out %u\n",
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
                      gEngineOwner.load() == this ? "owns" : "does NOT own",
                      (int)presets_.size(), loaded, loaded >= 0 && loaded < (int)presets_.size() ? presets_[(size_t)loaded].c_str() : "none",
                      loadError_.load(), loads_.load(), samplesLoaded_.load(), samplesFailed_.load(), statCalls_.load(), statSilentCalls_.load(), statNoteOns_.load(),
                      (double)statPeak_.load(), seconds, seconds > 0 ? 100.0 * ((double)ns * 1e-9) / seconds : 0.0,
                      (double)statWorstPermille_.load() * 0.1,
                      statMinBlock_.load() == INT32_MAX ? 0 : statMinBlock_.load(), statMaxBlock_.load(),
                      statClipped_.load(), (unsigned long long)(2 * frames),
                      frames > 0 ? 100.0 * (double)statClipped_.load() / (double)(2 * frames) : 0.0,
                      statSaves_.load(), statRestores_.load(), statRestoresWithoutKnobs_.load(), statWaitTimeouts_.load());
        if (FILE* f = std::fopen((folder + "/diagnostics.txt").c_str(), "w"))
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
    static constexpr int kNoKnobChange = -100000;
    std::atomic<int>  pendingKnob_[deluge_hosted::kNumKnobs];   // set from outside, not yet given to the engine
    std::atomic<int>  hostKnob_[deluge_hosted::kNumKnobs];      // the last value set from outside
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
    std::atomic<uint32_t> hostToTell_{0};    // parameters (one bit each) whose new value the MPC has yet to be told
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
    std::atomic<int>      statMinBlock_{INT32_MAX}, statMaxBlock_{0};
    std::mutex            diagnosticsLock_;
    int64_t               diagnosticsWrittenMs_ = 0;   // worker thread only
    uint32_t              diagnosticsCalls_ = 0;       // worker thread only
};

MPC_PLUGIN_ENTRY(DelugeSynth)
