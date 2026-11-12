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

// After a load: how many sample files the preset brought fully into memory, and how many it could
// not (file missing, or not enough memory). Samples are looked up as the preset names them,
// relative to the root, normally "SAMPLES/...".
void engineSampleCounts(int* loaded, int* failed);

void engineNoteOn(int note, int velocity); // velocity 1..127
void engineNoteOff(int note);
void engineAllNotesOff(); // releases every held note (they still ring out through their release)

// Channel-wide expression, as a Deluge treats it arriving on a synth's MIDI input channel: pitch
// bend (-8192..8191; the range is the preset's bend range), the mod wheel (0..127; the firmware
// routes it to its "Y" expression source) and channel pressure (0..127; its "Z" source). What they
// do depends on the preset's patching.
void enginePitchBend(int value);
void engineModWheel(int value);
void engineChannelPressure(int value);

// --- the gold knobs ---------------------------------------------------------------------------------
// A Deluge has two gold knobs and eight buttons that choose what the pair controls: sixteen
// assignments, stored in each preset. Knob numbers here run 0..15: (button 0, upper knob),
// (button 0, lower knob), (button 1, upper knob) and so on.
constexpr int kNumKnobs = 16;
constexpr int kKnobMin = -64, kKnobMax = 64; // the firmware's knob positions; it displays them as 0..50

// The name of what the knob controls in the loaded preset ("LPF frequency", "LFO1 -> pitch"), or
// an empty string if it controls nothing that has a value. Returns false if nothing is loaded.
bool engineKnobName(int knob, char* out, int outSize);
// The knob's position, kKnobMin..kKnobMax. Returns false if the knob has no value to read.
bool engineKnobGet(int knob, int* position);
void engineKnobSet(int knob, int position);

// Renders numFrames of interleaved stereo, replacing what is in `out`. numFrames must be a
// multiple of 4 from 4 to kMaxRenderFrames: the firmware always renders in fours.
// This is the firmware's whole output path: the sound, the song-level reverb fed by the sound's
// reverb send, the song's own effects and master volume, and the final gain stage. The values are
// what a Deluge sends to its converter: 32-bit, full scale = +/-2^31.
void engineRender(int32_t* out, int numFrames);

} // namespace deluge_hosted
