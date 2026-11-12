// Deluge Synth - an MPC instrument plugin by Fentonia
//
// The Synthstrom Deluge's synth engine, running the Deluge community firmware's own code: this
// file is only the plugin shell. It picks a SYNTH preset file, hands MIDI notes to the engine and
// copies the engine's output to the MPC. Everything that makes sound is in the engine
// (hosted/engine.h, built from the firmware into libdeluge.a).
//
// Limits of this first version:
//   * 32-bit MPCs only (Force, MPC Live, X, One, Key). Other builds contain a placeholder tone.
//   * One instance per project makes sound. The firmware keeps its state in globals, so a second
//     instance in the same MPC process stays silent.
//   * Presets come from Content/SYNTHS/ inside the plugin folder. Sample-based presets are silent.
//   * No tempo sync, pitch bend, mod wheel or gold-knob parameters yet.
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
    kNumParams
};

// One engine per process (see the header comment).
std::mutex gEngineLock;
std::atomic<void*> gEngineOwner{nullptr};

constexpr int kMaxPresets = 1024;

} // namespace

class DelugeSynth : public mpc::Plugin
{
public:
    DelugeSynth() : mpc::Plugin(makeInfo())
    {
        scanPresets();
        addParam(mpc::floatParam("volume", "Volume", -40.0f, 12.0f, 0.0f, "dB", 1));
        // The range is fixed so that projects keep working when presets are added or removed;
        // values past the last preset select the last one.
        addParam(mpc::intParam("preset", "Preset", 0, kMaxPresets - 1, 0));
    }

    ~DelugeSynth() override
    {
        quit_.store(true);
        if (worker_.joinable()) worker_.join();
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
            requestedPreset_.store(presetIndex());
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

    void parameterChanged(int index, float plainValue) override
    {
        (void)plainValue;
        if (index == kPreset) requestedPreset_.store(presetIndex());
    }

    bool formatValue(int index, float plainValue, std::string& out) override
    {
        if (index != kPreset) return false;
        if (presets_.empty()) { out = "(none)"; return true; }
        const int i = mpc::clamp((int)std::lround(plainValue), 0, (int)presets_.size() - 1);
        out = presets_[(size_t)i].substr(0, presets_[(size_t)i].size() - 4);   // without ".XML"
        return true;
    }

    // The project remembers the preset by its path under Content/SYNTHS, not by its position in
    // the list, so adding or removing preset files does not change what a project plays.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        const int i = presetIndex();
        if (i >= 0) out.assign(presets_[(size_t)i].begin(), presets_[(size_t)i].end());
        writeDiagnostics();   // a project save is a convenient moment to refresh the file
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        const std::string wanted(reinterpret_cast<const char*>(data), size);
        for (size_t i = 0; i < presets_.size(); ++i)
        {
            if (presets_[i] == wanted)
            {
                set(kPreset, (float)i);
                requestedPreset_.store((int)i);
                return;
            }
        }
        // Not found (the file was removed or renamed): keep whatever the Preset number selects.
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const auto timeStart = std::chrono::steady_clock::now();
        const int numFrames = ctx.numFrames;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        statCalls_.fetch_add(1, std::memory_order_relaxed);

        // Silence (the outputs are already zero) when this instance does not own the engine or a
        // preset is being loaded.
        if (gEngineOwner.load(std::memory_order_relaxed) != this || !gEngineLock.try_lock())
        {
            statSilentCalls_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (wantAllNotesOff_.exchange(false)) deluge_hosted::engineAllNotesOff();
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
    void scanPresets() { scanFolder(mpc::contentPath("SYNTHS"), "", 0); std::sort(presets_.begin(), presets_.end(), lessIgnoringCase); if (presets_.size() > (size_t)kMaxPresets) presets_.resize((size_t)kMaxPresets); }

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
            const int wanted = requestedPreset_.load();
            if (wanted >= 0 && wanted != loadedPreset_.load() && wanted < (int)presets_.size())
            {
                const std::string path = "SYNTHS/" + presets_[(size_t)wanted];
                std::lock_guard<std::mutex> lock(gEngineLock);
                loadError_.store(deluge_hosted::engineLoadSynth(path.c_str()));
                loadedPreset_.store(wanted);
                loads_.fetch_add(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }

    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn()) { deluge_hosted::engineNoteOn(e.note(), e.velocity()); statNoteOns_.fetch_add(1, std::memory_order_relaxed); }
        else if (e.isNoteOff()) deluge_hosted::engineNoteOff(e.note());
        else if (e.isAllNotesOff()) deluge_hosted::engineAllNotesOff();
    }

    // What happened so far, written into the plugin's own folder: there is no other way to see
    // inside the plugin on the device. `mpc_remote.py export` brings the folder back.
    void writeDiagnostics()
    {
        const std::string folder = mpc::pluginFolder();
        if (folder.empty()) return;
        char text[1200];
        const uint64_t frames = statFrames_.load(), ns = statNs_.load();
        const double seconds = (double)frames / 44100.0;
        const int loaded = loadedPreset_.load();
        std::snprintf(text, sizeof text,
                      "Deluge Synth diagnostics (written on prepare and on project/preset save)\n"
                      "engine: %s build; this instance %s the engine\n"
                      "presets found in Content/SYNTHS: %d\n"
                      "loaded preset: %d (%s), load error %d, loads so far %d\n"
                      "process calls %u, of which silent (no engine or loading) %u; note-ons %u; peak out %.4f\n"
                      "time in process(): %.2f s of audio rendered, average %.2f%% of one core, worst block %.1f%%\n",
#if defined(__arm__)
                      "Deluge firmware (32-bit ARM)",
#else
                      "PLACEHOLDER TONE (not 32-bit ARM)",
#endif
                      gEngineOwner.load() == this ? "owns" : "does NOT own",
                      (int)presets_.size(), loaded, loaded >= 0 && loaded < (int)presets_.size() ? presets_[(size_t)loaded].c_str() : "none",
                      loadError_.load(), loads_.load(), statCalls_.load(), statSilentCalls_.load(), statNoteOns_.load(),
                      (double)statPeak_.load(), seconds, seconds > 0 ? 100.0 * ((double)ns * 1e-9) / seconds : 0.0,
                      (double)statWorstPermille_.load() * 0.1);
        if (FILE* f = std::fopen((folder + "/diagnostics.txt").c_str(), "w"))
        {
            std::fputs(text, f);
            std::fclose(f);
        }
    }

    std::vector<std::string> presets_;
    std::thread       worker_;
    std::atomic<bool> quit_{false};
    std::atomic<int>  requestedPreset_{-1}, loadedPreset_{-1}, loadError_{0}, loads_{0};
    std::atomic<bool> wantAllNotesOff_{false};

    int32_t engineBuffer_[deluge_hosted::kMaxRenderFrames * 2];
    int     leftoverFrames_ = 0, leftoverStart_ = 0;
    float   smoothedGain_ = 1.0f;

    std::atomic<uint32_t> statCalls_{0}, statSilentCalls_{0}, statNoteOns_{0}, statWorstPermille_{0};
    std::atomic<uint64_t> statNs_{0}, statFrames_{0};
    std::atomic<float>    statPeak_{0.0f};
};

MPC_PLUGIN_ENTRY(DelugeSynth)
