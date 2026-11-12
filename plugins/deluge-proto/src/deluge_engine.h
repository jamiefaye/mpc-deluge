// Deluge Proto - the boundary between the plugin (DelugeProto.cpp) and Deluge firmware code
// (deluge_engine.cpp). Nothing from the firmware is visible through this header, so the plugin
// side does not see the firmware's global names and macros.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace dproto {

constexpr int kNumVoices = 8;
constexpr int kMaxChunk = 128; // the Deluge renders in blocks of at most 128 samples

struct Params {
    int oscType = 4;          // index into kOscNames
    float pulseWidth = 0.0f;  // 0..1, square waves only
    float cutoffHz = 4000.0f;
    float resonance = 0.0f;   // 0..1
    int filterMode = 1;       // index into kFilterNames
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void resetAll();
    void noteOn(int voice, float hz);
    // Adds numFrames (<= kMaxChunk) of one voice to out, scaled linearly from gainStart to gainEnd.
    void render(int voice, float* out, int numFrames, const Params& params, float gainStart, float gainEnd);

    // Diagnostic: renders one voice offline for each oscillator type and filter mode with a private
    // voice (the playing voices are untouched) and appends what each stage produced to `report`.
    // Allocates; not for the audio thread. Exists because the 32-bit ARM build cannot be run on
    // the development machine: the plugin writes this report into its own folder on the MPC.
    void selfTest(std::string& report);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace dproto
