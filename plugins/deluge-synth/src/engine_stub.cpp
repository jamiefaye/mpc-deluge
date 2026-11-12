// Placeholder engine for the builds that cannot contain the real one.
//
// The Deluge firmware only builds for 32-bit ARM so far (it keeps pointers in 32-bit fields). The
// SDK's build flow still needs this plugin to compile and run on the development computer (for the
// parameter list, the generated screen and the host simulator) and for 64-bit MPCs. On those
// targets the engine calls below are answered by a plain sine tone, so the tools have a plugin
// that loads and makes a sound. It is not the Deluge and is not meant to be listened to.
//
// On 32-bit ARM this file compiles to nothing and the real engine comes from libdeluge.a.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#if !defined(__arm__)

#include "engine.h"

#include <cmath>
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
