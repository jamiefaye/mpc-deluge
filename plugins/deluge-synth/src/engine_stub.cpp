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
Tone tones[8];
} // namespace

void engineInit(const char*) {}

int engineLoadSynth(const char*) {
	for (Tone& t : tones) {
		t = Tone();
	}
	return 0;
}

void engineSampleCounts(int* loaded, int* failed) {
	*loaded = 0;
	*failed = 0;
}

bool engineKnobName(int knob, char* out, int outSize) {
	std::snprintf(out, (size_t)outSize, "placeholder %d", knob + 1);
	return true;
}

bool engineKnobGet(int, int* position) {
	*position = 0;
	return true;
}

void engineKnobSet(int, int) {}

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
