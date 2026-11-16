// The Deluge synth engine as a host program sees it: bring the firmware up, load one SYNTH
// preset, send notes, render audio. Nothing from the firmware is visible through this header.
//
// One engine per process: the firmware keeps its state in globals (see CLAUDE.md, "Globals").
// It can hold several sounds at once, each an "instance" with its own song (so its own reverb and
// master stage); every function below acts on the instance last chosen with engineUse().
//
// Threads: none of this is thread-safe. The caller makes sure only one thread is inside any of
// these functions at a time (the plugin holds a lock around every call). Other threads may run
// freely outside them: memory they allocate with new does not come from the firmware's allocator
// (hosted/operators.cpp), which is the one thing in here every thread would otherwise share.
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

// --- instances ---------------------------------------------------------------------------------------
// engineInit() makes instance 0 and leaves it current, which is all a program playing one sound
// needs. A plugin makes one per plugin instance and calls engineUse() each time it enters the
// engine. Instances share the voice pools, the sample files and the memory, and nothing they play
// reaches one another: each renders exactly what it would alone.
constexpr int kMaxInstances = 16;
// Makes a new instance, with nothing loaded, and makes it current. Returns its number, or -1 if
// there are kMaxInstances already or no memory.
int engineNewInstance();
// Makes `instance` the one the other functions act on. Cheap enough to call before every use.
void engineUse(int instance);
// Stops and frees an instance (not 0). Instance 0 is current afterwards.
void engineDeleteInstance(int instance);
int engineCurrentInstance();

// Loads a SYNTH preset (path relative to the root, e.g. "SYNTHS/SYNT100.XML") into a new clip, the
// way the firmware sets up a song's first clip. Returns 0, or the firmware's Error code.
// May be called again to change preset: all sound stops and the song (with its reverb) is rebuilt.
// Not thread-safe against the other calls: the caller serialises them.
int engineLoadSynth(const char* presetPath);

// Writes the loaded sound, as it now stands, to a SYNTH preset file a Deluge can load (path
// relative to the root, e.g. "SYNTHS/SYNT100 2.XML"), with the firmware's own writer: what saving
// a synth on a Deluge writes. Will not overwrite: returns the firmware's FILE_ALREADY_EXISTS.
// Writes a file: not for the audio thread. Returns 0 or the firmware's Error code.
int engineSaveSynth(const char* presetPath);

// After a load: how many sample files the preset brought fully into memory, and how many it could
// not (file missing, or not enough memory). Samples are looked up as the preset names them,
// relative to the root, normally "SAMPLES/...".
void engineSampleCounts(int* loaded, int* failed);

// How many times the firmware has stopped at one of its own consistency checks (on a Deluge: an
// error code on the display, and a button to carry on) since the engine started, and the latest
// code. Each is a bug, in the firmware or in how it is driven here; the engine carries on.
int engineFirmwareErrors(char* latest, int size);

// --- the oscillators' samples -------------------------------------------------------------------------
// Oscillator `source` (0 or 1) plays the file at `path` (relative to the root, e.g.
// "SAMPLES/Kicks/909.wav") as a sample, as choosing it in the Deluge's sample browser does: the
// sound stops, the oscillator becomes a sample oscillator, its first key zone takes the file,
// and the play mode follows the file (loop points and length) unless it was Loop or Stretch
// already. Reads the file: not for the audio thread. Returns 0 or the firmware's Error code; after
// an error the zone has no file. An empty path takes the file away and leaves the zone empty.
// Returns -1, and changes nothing, unless the sound's synth mode is Subtractive: the firmware
// offers samples in no other.
int engineSampleSet(int source, const char* path);
// The file of the oscillator's first key zone. False (and an empty string) if it is not a sample
// oscillator or has no file.
bool engineSampleGet(int source, char* out, int outSize);
// A number that stands for that file: engineSampleId(path) of the same path, 0 for none. Cheap,
// for the audio thread to notice that an oscillator's file has changed or gone.
uint32_t engineSampleIdOf(int source);
inline uint32_t engineSampleId(const char* path) {
	uint32_t h = 2166136261u; // FNV-1a over the path, letters lowered
	for (; *path != 0; path++) {
		const unsigned char c = (unsigned char)*path;
		h = (h ^ (uint32_t)((c >= 'A' && c <= 'Z') ? c + 32 : c)) * 16777619u;
	}
	return h != 0 ? h : 1;
}

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
// A knob on a modulation depth ("LFO1 -> pitch") reads kKnobMin while the sound has no such
// patch cable; setting it higher creates the cable, as turning the knob on a Deluge does.
bool engineKnobGet(int knob, int* position);
void engineKnobSet(int knob, int position);

// Renders numFrames of interleaved stereo, replacing what is in `out`. numFrames must be a
// multiple of 4 from 4 to kMaxRenderFrames: the firmware always renders in fours.
// This is the firmware's whole output path: the sound, the song-level reverb fed by the sound's
// reverb send, the song's own effects and master volume, and the final gain stage. The values are
// what a Deluge sends to its converter: 32-bit, full scale = +/-2^31.
void engineRender(int32_t* out, int numFrames);

} // namespace deluge_hosted
