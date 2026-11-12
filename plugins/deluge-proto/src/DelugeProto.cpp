// Deluge Proto - an MPC instrument plugin by Fentonia
//
// A proof that Synthstrom Deluge firmware DSP runs inside an MPC plugin. It is not the Deluge
// synth engine: only the oscillators and the filter set are the firmware's code (see
// deluge_engine.cpp). The voice handling, the envelope and the parameter scaling in this file
// are simple stand-ins written for the prototype.
//
// Signal flow, per voice:
//   Deluge oscillator (fixed point) -> Deluge filter set (low-pass side) -> float
//   -> ADSR envelope * velocity (this file) -> sum of voices -> volume -> both outputs
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mpc_plugin.h"

#include "deluge_engine.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

// The order here is permanent once a project uses the plugin, and must match the addParam() calls.
enum ParamIndex
{
    kVolume = 0,     // dB. First on purpose: the MPC nudges parameter 0 on every load.
    kWaveform,       // Deluge oscillator type
    kPulseWidth,     // %, square waves only
    kCutoff,         // Hz
    kResonance,      // %
    kFilterMode,     // Deluge low-pass filter mode
    kAttack,         // ms
    kDecay,          // ms
    kSustain,        // %
    kRelease,        // ms
    kNumParams
};

// Linear-segment ADSR, advanced once per render chunk.
struct Envelope
{
    enum Stage { Idle, Attack, Decay, Sustain, Release };
    Stage stage = Idle;
    float level = 0.0f;

    void noteOn()  { stage = Attack; }
    void noteOff() { if (stage != Idle) stage = Release; }
    bool isActive() const { return stage != Idle; }

    // Moves the envelope forward by numFrames and returns the new level.
    float advance(int numFrames, float attackMs, float decayMs, float sustain, float releaseMs, float sampleRate)
    {
        const float n = (float)numFrames;
        switch (stage)
        {
            case Attack:
                level += n / std::fmax(1.0f, attackMs * 0.001f * sampleRate);
                if (level >= 1.0f) { level = 1.0f; stage = Decay; }
                break;
            case Decay:
                level -= n * (1.0f - sustain) / std::fmax(1.0f, decayMs * 0.001f * sampleRate);
                if (level <= sustain) { level = sustain; stage = Sustain; }
                break;
            case Sustain:
                level = sustain;
                break;
            case Release:
                level -= n / std::fmax(1.0f, releaseMs * 0.001f * sampleRate);
                if (level <= 0.0f) { level = 0.0f; stage = Idle; }
                break;
            case Idle:
                level = 0.0f;
                break;
        }
        return level;
    }
};

struct Voice
{
    int      note = -1;
    float    velocity = 0.0f;
    Envelope env;
    uint32_t startOrder = 0;   // for voice stealing (oldest first)
};

} // namespace

class DelugeProto : public mpc::Plugin
{
public:
    DelugeProto() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam ("volume",    "Volume",    -40.0f, 6.0f, -6.0f, "dB", 1));
        addParam(mpc::choiceParam("wave",      "Waveform",
                                  {"Sine", "Triangle", "Square", "Ana Square", "Saw", "Ana Saw"}, 4));
        addParam(mpc::floatParam ("pw",        "Pulse Width", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::logParam   ("cutoff",    "Cutoff",    20.0f, 20000.0f, 2500.0f, "Hz", 0));
        addParam(mpc::floatParam ("resonance", "Resonance", 0.0f, 100.0f, 30.0f, "%", 0));
        addParam(mpc::choiceParam("fmode",     "Filter",
                                  {"12dB", "24dB", "Drive", "SVF Band", "SVF Notch", "Off"}, 1));
        addParam(mpc::logParam   ("attack",    "Attack",    1.0f, 5000.0f, 3.0f,   "ms", 0));
        addParam(mpc::logParam   ("decay",     "Decay",     1.0f, 5000.0f, 300.0f, "ms", 0));
        addParam(mpc::floatParam ("sustain",   "Sustain",   0.0f, 100.0f,  70.0f,  "%",  0));
        addParam(mpc::logParam   ("release",   "Release",   1.0f, 5000.0f, 250.0f, "ms", 0));

        //                    vol wave pw  cutoff res mode att  dec  sus rel
        addProgram("Init Saw",   {-6, 4, 0,  2500, 30, 1, 3,   300, 70, 250});
        addProgram("Ladder Bass",{-6, 5, 0,  500,  65, 1, 1,   250, 40, 120});
        addProgram("Drive Lead", {-9, 2, 30, 1800, 55, 2, 5,   400, 80, 300});
        addProgram("Soft Pad",   {-8, 1, 0,  1200, 10, 0, 600, 900, 85, 1200});
        addProgram("Notch Pluck",{-6, 3, 20, 3000, 40, 4, 1,   180, 0,  200});
    }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        (void)maxBlockSize;
        sampleRate_ = (float)sampleRate;   // the MPC host is fixed at 44100 Hz, as is the Deluge
        reset();
        writeDiagnostics(sampleRate, maxBlockSize);
    }

    // Diagnostics for the 32-bit ARM build, which cannot be run on the development machine. Called
    // from prepare() (not the audio thread): writes what the engine computes offline, and what
    // process() has seen so far, to diagnostics.txt in the plugin's own folder. The MPC calls
    // prepare() again on every suspend/resume, so the file is refreshed with the latest counts.
    void writeDiagnostics(double sampleRate, int maxBlockSize)
    {
        const std::string folder = mpc::pluginFolder();
        if (folder.empty()) return;
        std::string text;
        char line[300];
        std::snprintf(line, sizeof line, "Deluge Proto diagnostics\nwrite #%d (on load, on each prepare, and on project/preset save): sampleRate=%.1f maxBlockSize=%d\n",
                      ++prepareCount_, sampleRate, maxBlockSize);
        text += line;
        std::snprintf(line, sizeof line,
                      "since load: process calls=%u frames=%u midi events=%u note-ons=%u peak out=%.6f volume param=%.2f dB "
                      "smoothed gain=%.4f numOutputs seen=%d\n",
                      statCalls_.load(), statFrames_.load(), statMidi_.load(), statNoteOns_.load(), (double)statPeak_.load(),
                      (double)get(kVolume), (double)smoothedGain_, statNumOutputs_.load());
        text += line;
        text += "--- time inside process(), as a share of one core, by voices sounding ---\n";
        for (int n = 0; n <= kNumVoices; ++n)
        {
            const uint64_t ns = timeBuckets_[n].ns.load(), frames = timeBuckets_[n].frames.load();
            if (frames == 0) continue;
            const double seconds = (double)frames / 44100.0;
            std::snprintf(line, sizeof line, "%d voice(s): %7.2f s of audio, average %.2f%%, worst block %.1f%%\n",
                          n, seconds, 100.0 * ((double)ns * 1e-9) / seconds, (double)timeBuckets_[n].worstPermille.load() * 0.1);
            text += line;
        }
        text += "--- engine self-test (offline, C4, cutoff 2500 Hz, resonance 30%) ---\n";
        engine_.selfTest(text);
        if (FILE* f = std::fopen((folder + "/diagnostics.txt").c_str(), "w"))
        {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }

    void reset() override
    {
        for (Voice& v : voices_) v = Voice();
        engine_.resetAll();
        noteCounter_ = 0;
        smoothedGain_ = dbToGain(get(kVolume));
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int numFrames = ctx.numFrames;
        const auto timeStart = std::chrono::steady_clock::now();
        int voicesAtStart = 0;
        for (const Voice& v : voices_) if (v.env.isActive()) ++voicesAtStart;
        statCalls_.fetch_add(1, std::memory_order_relaxed);
        statFrames_.fetch_add((uint32_t)numFrames, std::memory_order_relaxed);
        statMidi_.fetch_add((uint32_t)ctx.numMidiEvents, std::memory_order_relaxed);
        statNumOutputs_.store(ctx.numOutputs, std::memory_order_relaxed);
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;

        dproto::Params p;
        p.oscType    = getInt(kWaveform);
        p.pulseWidth = get(kPulseWidth) * 0.01f;
        p.cutoffHz   = get(kCutoff);
        p.resonance  = get(kResonance) * 0.01f;
        p.filterMode = getInt(kFilterMode);
        const float attackMs  = get(kAttack);
        const float decayMs   = get(kDecay);
        const float sustain   = get(kSustain) * 0.01f;
        const float releaseMs = get(kRelease);
        const float targetGain = dbToGain(get(kVolume));

        // Walk through the block in chunks that end at MIDI events and never exceed the Deluge's
        // own render size.
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
            if (segmentEnd - frame > dproto::kMaxChunk)
                segmentEnd = frame + dproto::kMaxChunk;
            const int n = segmentEnd - frame;

            for (int i = 0; i < kNumVoices; ++i)
            {
                Voice& v = voices_[i];
                if (!v.env.isActive()) continue;
                const float before = v.env.level * v.velocity;
                const float after  = v.env.advance(n, attackMs, decayMs, sustain, releaseMs, sampleRate_) * v.velocity;
                engine_.render(i, outL + frame, n, p, before, after);
            }

            // Master volume, smoothed so knob moves do not click; copy to the right channel.
            for (int i = frame; i < segmentEnd; ++i)
            {
                smoothedGain_ += 0.002f * (targetGain - smoothedGain_);
                outL[i] *= smoothedGain_;
                if (outR) outR[i] = outL[i];
            }
            frame = segmentEnd;
        }

        float peak = statPeak_.load(std::memory_order_relaxed);
        for (int i = 0; i < numFrames; ++i)
        {
            const float a = std::fabs(outL[i]);
            if (a > peak) peak = a;
        }
        statPeak_.store(peak, std::memory_order_relaxed);

        // Time spent in this call, filed under the number of voices that were sounding when it
        // started (a note-on inside the block is counted one voice low; good enough for a meter).
        const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - timeStart).count();
        TimeBucket& b = timeBuckets_[voicesAtStart];
        b.ns.fetch_add(ns, std::memory_order_relaxed);
        b.frames.fetch_add((uint64_t)numFrames, std::memory_order_relaxed);
        const uint32_t permille = numFrames > 0 ? (uint32_t)(ns * 441ull / (10000ull * (uint64_t)numFrames)) : 0u;   // ns / (frames/44100 s) * 1000
        if (permille > b.worstPermille.load(std::memory_order_relaxed)) b.worstPermille.store(permille, std::memory_order_relaxed);
    }

    // Project or preset save (not the audio thread): a convenient moment to refresh diagnostics.txt
    // with the timing gathered so far. Nothing extra is stored in the state.
    void saveExtraState(std::vector<uint8_t>& out) override
    {
        (void)out;
        writeDiagnostics(sampleRate_, 0);
    }

private:
    static constexpr int kNumVoices = dproto::kNumVoices;

    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name         = "Deluge Proto";
        info.vendor       = "Fentonia";   // must match plugin.json "manufacturer"
        info.uniqueId     = MPC_FOURCC(kUid[0], kUid[1], kUid[2], kUid[3]);   // must match plugin.json "uid"
        info.version      = 1000;         // 1.0.0.0
        info.isInstrument = true;
        info.numInputs    = 0;
        info.numOutputs   = 2;
        info.tailSamples  = 0;
        return info;
    }

    static constexpr char kUid[5] = "FT6q";

    static float dbToGain(float dB) { return std::pow(10.0f, dB / 20.0f); }

    void handleMidi(const mpc::MidiEvent& e)
    {
        if (e.isNoteOn())
        {
            statNoteOns_.fetch_add(1, std::memory_order_relaxed);
            const int index = findFreeVoice();
            Voice& v = voices_[index];
            v.note = e.note();
            v.velocity = e.velocity() / 127.0f;
            v.startOrder = ++noteCounter_;
            v.env.level = 0.0f;
            v.env.noteOn();
            engine_.noteOn(index, mpc::noteToHz((float)e.note()));
        }
        else if (e.isNoteOff())
        {
            for (Voice& v : voices_)
                if (v.note == e.note() && v.env.isActive()) v.env.noteOff();
        }
        else if (e.isAllNotesOff())
        {
            for (Voice& v : voices_) v.env.noteOff();
        }
    }

    int findFreeVoice()
    {
        int best = 0;
        for (int i = 0; i < kNumVoices; ++i)
        {
            if (!voices_[i].env.isActive()) return i;
            if (voices_[i].startOrder < voices_[best].startOrder) best = i;   // steal the oldest
        }
        return best;
    }

    dproto::Engine engine_;
    Voice    voices_[kNumVoices];
    float    sampleRate_ = 44100.0f;
    float    smoothedGain_ = 0.5f;
    uint32_t noteCounter_ = 0;

    // Diagnostics (see writeDiagnostics).
    int prepareCount_ = 0;
    std::atomic<uint32_t> statCalls_{0}, statFrames_{0}, statMidi_{0}, statNoteOns_{0};
    std::atomic<float>    statPeak_{0.0f};
    std::atomic<int>      statNumOutputs_{0};
    struct TimeBucket { std::atomic<uint64_t> ns{0}, frames{0}; std::atomic<uint32_t> worstPermille{0}; };
    TimeBucket timeBuckets_[kNumVoices + 1];
};

// This macro creates the exported VSTPluginMain entry point. Keep it last.
MPC_PLUGIN_ENTRY(DelugeProto)
