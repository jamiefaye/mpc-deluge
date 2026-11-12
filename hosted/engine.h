// The Deluge synth engine as a host program sees it: bring the firmware up, load one SYNTH
// preset, send notes, render audio. Nothing from the firmware is visible through this header.
//
// One engine per process: the firmware keeps its state in globals (see CLAUDE.md, "Globals").
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace deluge_hosted {

constexpr int kMaxRenderFrames = 128; // the firmware's own render size

// Starts the firmware (allocator, audio engine, default settings, an empty song). `root` is the
// folder that stands in for the Deluge's SD card; preset and sample paths are relative to it.
// Safe to call again; later calls only change the root.
void engineInit(const char* root);

// Loads a SYNTH preset (path relative to the root, e.g. "SYNTHS/SYNT100.XML") into a new clip, the
// way the firmware sets up a song's first clip. Returns 0, or the firmware's Error code.
// Not yet: unloading or replacing a loaded preset; call once per process.
int engineLoadSynth(const char* presetPath);

void engineNoteOn(int note, int velocity); // velocity 1..127
void engineNoteOff(int note);

// Renders numFrames (1..kMaxRenderFrames) of interleaved stereo, in the firmware's own 32-bit
// fixed-point scale, replacing what is in `out`. The song-level reverb is not mixed in yet: a
// preset's reverb send goes nowhere.
void engineRender(int32_t* out, int numFrames);

} // namespace deluge_hosted
