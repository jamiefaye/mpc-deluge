// Placeholder engine for the builds that cannot contain the real one.
//
// The real engine is the firmware library, linked into the builds for 32-bit and 64-bit ARM MPCs
// and into the native build on a 64-bit ARM computer (tools/build.py defines DELUGE_ENGINE_LINKED
// for those, and this file then compiles to nothing). The firmware's signal processing is written
// with ARM NEON, so it cannot be compiled for an Intel processor: the native build on an Intel
// computer and the desktop VST (a universal binary) get the engine calls below instead, answered
// by a plain sine tone, so that the SDK's tools still have a plugin that loads and makes a sound.
// It is not the Deluge and is not meant to be listened to.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#if !defined(DELUGE_ENGINE_LINKED)

#include "engine.h"
#include "matrix.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace deluge_hosted {

namespace {
struct Tone {
	int note = -1;
	bool held = false;
	float level = 0.0f, phase = 0.0f;
};
// No engine to hold them, so the placeholder remembers what the knobs and grid pads were set to:
// the plugin reads them back after every change and expects to find them. One set per instance.
struct Placeholder {
	bool inUse = false;
	Tone tones[8];
	int knobs[kNumKnobs] = {};
	int32_t pads[kGridSlots] = {};
};
Placeholder placeholders[kMaxInstances];
// The placeholder remembers the name so that the plugin's File parameters behave.
char sampleNames[kMaxInstances][2][256];
int current = 0;
#define tones (placeholders[current].tones)
#define knobs (placeholders[current].knobs)
#define pads (placeholders[current].pads)
} // namespace

void engineInit(const char*) {
	placeholders[0].inUse = true;
}

int engineNewInstance() {
	for (int i = 1; i < kMaxInstances; i++) {
		if (!placeholders[i].inUse) {
			placeholders[i] = Placeholder();
			placeholders[i].inUse = true;
			current = i;
			return i;
		}
	}
	return -1;
}

void engineUse(int instance) {
	if (instance >= 0 && instance < kMaxInstances && placeholders[instance].inUse) {
		current = instance;
	}
}

void engineDeleteInstance(int instance) {
	if (instance >= 1 && instance < kMaxInstances) {
		placeholders[instance].inUse = false;
		current = 0;
	}
}

int engineCurrentInstance() {
	return current;
}

int engineLoadSynth(const char*) {
	sampleNames[current][0][0] = sampleNames[current][1][0] = 0;
	for (Tone& t : tones) {
		t = Tone();
	}
	return 0;
}

int engineFirmwareErrors(char* latest, int size) {
	if (size > 0) {
		latest[0] = 0;
	}
	return 0;
}

int engineSampleSet(int source, const char* path) {
	if (source < 0 || source > 1) {
		return 1;
	}
	std::snprintf(sampleNames[current][source], sizeof sampleNames[0][0], "%s", path);
	return 0;
}
bool engineSampleGet(int source, char* out, int outSize) {
	if (outSize > 0) {
		out[0] = 0;
	}
	if (source < 0 || source > 1 || sampleNames[current][source][0] == 0) {
		return false;
	}
	std::snprintf(out, (size_t)outSize, "%s", sampleNames[current][source]);
	return true;
}

uint32_t engineSampleIdOf(int source) {
	return (source < 0 || source > 1 || sampleNames[current][source][0] == 0) ? 0 : engineSampleId(sampleNames[current][source]);
}

int engineSaveSynth(const char*) {
	return 1; // nothing to write: there is no sound
}

void engineSampleCounts(int* loaded, int* failed) {
	*loaded = 0;
	*failed = 0;
}

bool engineKnobName(int knob, char* out, int outSize) {
	std::snprintf(out, (size_t)outSize, "placeholder %d", knob + 1);
	return true;
}

bool engineKnobGet(int knob, int* position) {
	if (knob < 0 || knob >= kNumKnobs) {
		return false;
	}
	*position = knobs[knob];
	return true;
}

void engineKnobSet(int knob, int position) {
	if (knob >= 0 && knob < kNumKnobs) {
		knobs[knob] = position;
	}
}

void engineCellSet(int layer, int x, int y, int32_t value) {
	if (layer >= 0 && layer < kGridLayers && x >= 0 && x < kGridColumns && y >= 0 && y < kGridRows) {
		pads[gridSlot(layer, x, y)] = value;
	}
}

void engineGridRead(int32_t* values, uint8_t* relevant) {
	std::memcpy(values, pads, sizeof pads);
	std::memset(relevant, 1, kGridSlots);
}

void enginePitchBend(int) {}
void engineModWheel(int) {}
void engineChannelPressure(int) {}

void engineNoteOn(int note, int velocity) {
	Tone* slot = &tones[0];
	for (Tone& t : tones) {
		if (t.level <= 0.0f) {
			slot = &t;
			break;
		}
	}
	slot->note = note;
	slot->held = true;
	slot->level = 0.1f * (float)velocity / 127.0f;
	slot->phase = 0.0f;
}

void engineNoteOff(int note) {
	for (Tone& t : tones) {
		if (t.note == note) {
			t.held = false;
		}
	}
}

void engineAllNotesOff() {
	for (Tone& t : tones) {
		t.held = false;
	}
}

void engineRender(int32_t* out, int numFrames) {
	std::memset(out, 0, sizeof(int32_t) * 2 * (size_t)numFrames);
	for (Tone& t : tones) {
		if (t.level <= 0.0f) {
			continue;
		}
		const float step = 440.0f * std::pow(2.0f, (float)(t.note - 69) / 12.0f) / 44100.0f;
		for (int i = 0; i < numFrames; i++) {
			if (!t.held) {
				t.level -= 0.0005f; // about 5 ms
				if (t.level <= 0.0f) {
					t.level = 0.0f;
					break;
				}
			}
			const int32_t v = (int32_t)(std::sin(6.2831853f * t.phase) * t.level * 2147483647.0f);
			out[2 * i] += v;
			out[2 * i + 1] += v;
			t.phase += step;
			t.phase -= std::floor(t.phase);
		}
	}
}

} // namespace deluge_hosted

#endif
