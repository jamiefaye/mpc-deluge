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
// May be called again to change preset: all sound stops and the song (with its reverb) is rebuilt.
// Not thread-safe against the other calls: the caller serialises them.
int engineLoadSynth(const char* presetPath);

void engineNoteOn(int note, int velocity); // velocity 1..127
void engineNoteOff(int note);
void engineAllNotesOff(); // releases every held note (they still ring out through their release)

// Renders numFrames of interleaved stereo, replacing what is in `out`. numFrames must be a
// multiple of 4 from 4 to kMaxRenderFrames: the firmware always renders in fours.
// This is the firmware's whole output path: the sound, the song-level reverb fed by the sound's
// reverb send, the song's own effects and master volume, and the final gain stage. The values are
// what a Deluge sends to its converter: 32-bit, full scale = +/-2^31.
void engineRender(int32_t* out, int numFrames);

} // namespace deluge_hosted
