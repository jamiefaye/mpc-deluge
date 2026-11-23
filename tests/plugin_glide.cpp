// A small VST2 host that loads the built Downrush plugin and checks that a pad's parameter can
// stand between two steps and glides to a value set from outside, which is what lets the MPC's
// automation play a Deluge's glide (DelugeSynth.cpp, "Pads on their way to a value from outside").
// The sound is a steady sine carried in a state, and its level is the measure.
//
//   - Master Level set half way between two steps plays at a level between theirs
//   - a large change arrives as a ramp over several blocks, not in one
//   - once there, on a whole step, the level is the step's own and the parameter shows it
//   - a choice (Synth Mode) is still set at once, as a whole
//
//   tools/plugintest.sh           (builds this and runs it against the native plugin build)
//
// Arguments: <plugin library>. Needs SYNT008 in the plugin's Content/SYNTHS (any preset would do).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "grid_table.h"
#include "mpc_vst2_abi.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>

static VstIntPtr hostCallback(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void* ptr, float) {
	switch (opcode) {
	case audioMasterVersion:
		return 2400;
	case audioMasterGetVendorString:
	case audioMasterGetProductString:
		if (ptr != nullptr) {
			std::strcpy(static_cast<char*>(ptr), "MPC 3");
		}
		return 1;
	case audioMasterGetSampleRate:
		return 44100;
	case audioMasterGetBlockSize:
		return 128;
	default:
		return 0;
	}
}

static AEffect* fx;
static int failures = 0;

static VstIntPtr disp(VstInt32 opcode, VstInt32 index = 0, VstIntPtr value = 0, void* ptr = nullptr, float opt = 0.0f) {
	return fx->dispatcher(fx, opcode, index, value, ptr, opt);
}
static int paramCalled(const char* name) {
	for (int i = 0; i < fx->numParams; i++) {
		char text[64] = {0};
		disp(effGetParamName, i, 0, text);
		if (std::strcmp(text, name) == 0) {
			return i;
		}
	}
	std::printf("FAIL  no parameter called %s\n", name);
	failures++;
	return 0;
}
static std::string shown(const char* name) {
	char text[64] = {0};
	disp(effGetParamDisplay, paramCalled(name), 0, text);
	return text;
}
static void check(bool ok, const char* what, const std::string& detail = "") {
	std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
	if (!ok) {
		failures++;
	}
}
// One block of 128 frames; returns its peak.
static float block() {
	static float left[128], right[128];
	float* outs[2] = {left, right};
	fx->processReplacing(fx, nullptr, outs, 128);
	float peak = 0.0f;
	for (float v : left) {
		peak = std::max(peak, std::fabs(v));
	}
	return peak;
}
// Audio for a while, in real time enough for the plugin's worker thread to do its part.
static void run(int milliseconds = 120) {
	for (int ms = 0; ms < milliseconds; ms += 3) {
		block();
		std::this_thread::sleep_for(std::chrono::milliseconds(3));
	}
}
static float level() { // the settled peak, over 16 blocks
	float peak = 0.0f;
	for (int i = 0; i < 16; i++) {
		peak = std::max(peak, block());
	}
	return peak;
}
static void send(int status, int key, int velocity) {
	struct {
		VstInt32 numEvents;
		VstIntPtr reserved;
		VstEvent* events[2];
	} list = {};
	VstMidiEvent event = {};
	event.type = kVstMidiType;
	event.byteSize = sizeof(VstMidiEvent);
	event.midiData[0] = (char)status;
	event.midiData[1] = (char)key;
	event.midiData[2] = (char)velocity;
	list.numEvents = 1;
	list.events[0] = reinterpret_cast<VstEvent*>(&event);
	disp(effProcessEvents, 0, 0, &list);
}
static void append32(std::vector<uint8_t>& to, uint32_t value) {
	for (int i = 0; i < 4; i++) {
		to.push_back((uint8_t)(value >> (8 * i)));
	}
}

static const char* kTone = R"(<?xml version="1.0" encoding="UTF-8"?>
<sound firmwareVersion="c1.3.0" earliestCompatibleFirmware="4.1.0-alpha" polyphonic="poly" mode="subtractive" lpfMode="24dB" modFXType="none">
	<osc1 type="sine" transpose="0" cents="0" retrigPhase="0" />
	<osc2 type="sine" transpose="0" cents="0" retrigPhase="0" />
	<defaultParams oscAVolume="0x7FFFFFFF" oscBVolume="0x80000000" noiseVolume="0x80000000" volume="0x4CCCCCA8" pan="0x00000000" lpfFrequency="0x7FFFFFFF" lpfResonance="0x80000000" hpfFrequency="0x80000000" hpfResonance="0x80000000" reverbAmount="0x80000000" delayFeedback="0x80000000">
		<envelope1 attack="0x80000000" decay="0x80000000" sustain="0x7FFFFFFF" release="0x80000000" />
	</defaultParams>
</sound>
)";

int main(int argc, char** argv) {
	if (argc < 2) {
		std::printf("usage: plugin_glide <plugin library>\n");
		return 1;
	}
	void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (library == nullptr) {
		std::printf("FAIL  %s\n", dlerror());
		return 1;
	}
	using Entry = AEffect* (*)(audioMasterCallback);
	Entry entry = reinterpret_cast<Entry>(dlsym(library, "VSTPluginMain"));
	fx = entry != nullptr ? entry(hostCallback) : nullptr;
	if (fx == nullptr) {
		std::printf("FAIL  no plugin in %s\n", argv[1]);
		return 1;
	}
	disp(effSetSampleRate, 0, 0, nullptr, 44100.0f);
	disp(effSetBlockSize, 0, 128);
	disp(effOpen);
	disp(effMainsChanged, 0, 1);
	disp(effStartProcess);
	run(300);

	// The tone, in a state as the converter writes one.
	std::vector<uint8_t> extra{0, 0, 'X', 1, 0, 1};
	const char* name = "Glide Tone";
	extra.insert(extra.end(), name, name + std::strlen(name) + 1);
	append32(extra, (uint32_t)std::strlen(kTone));
	extra.insert(extra.end(), kTone, kTone + std::strlen(kTone));
	std::vector<uint8_t> state{'M', 'P', 'C', 'P'};
	append32(state, 2);
	append32(state, 0);
	append32(state, (uint32_t)extra.size());
	state.insert(state.end(), extra.begin(), extra.end());
	disp(effSetChunk, 0, (VstIntPtr)state.size(), state.data());
	run(400);
	check(shown("Preset") == name, "the tone loads from a state", shown("Preset"));

	const int master = paramCalled("Master Level");
	auto setSteps = [&](float steps) { fx->setParameter(fx, master, steps / 50.0f); };
	send(0x90, 84, 100); // C6: a period of 42 frames, so a block's peak is the level
	setSteps(20.0f);
	run(200);
	const float at20 = level();
	setSteps(21.0f);
	run(200);
	const float at21 = level();
	setSteps(20.5f);
	run(200);
	const float between = level();
	char text[160];
	std::snprintf(text, sizeof text, "step 20 %.5f, 20.5 %.5f, step 21 %.5f", at20, between, at21);
	check(at20 > 0.0005f && at21 > at20 * 1.01f, "a step of Master Level is a step in level", text);
	check(between > at20 * 1.003f && between < at21 * 0.997f, "half way between two steps plays between their levels", text);

	// A large change: ten steps to forty. The level climbs over the blocks that follow.
	setSteps(10.0f);
	run(200);
	const float low = level();
	setSteps(40.0f);
	float peaks[24];
	for (float& peak : peaks) {
		peak = block();
	}
	run(200);
	const float high = level();
	int rising = 0, partWay = 0;
	std::string trace;
	for (int i = 0; i < 24; i++) {
		if (i > 0 && peaks[i] > peaks[i - 1] * 1.02f) {
			rising++;
		}
		if (peaks[i] > low * 1.2f && peaks[i] < high * 0.8f) {
			partWay++;
		}
		if (i < 12) {
			std::snprintf(text, sizeof text, "%s%.4f", i ? " " : "", peaks[i]);
			trace += text;
		}
	}
	std::snprintf(text, sizeof text, "from %.5f to %.5f; %d blocks part way, %d rises; first twelve blocks: ", low, high, partWay, rising);
	check(high > low * 4.0f && partWay >= 3 && rising >= 4, "a large change arrives as a ramp over several blocks", text + trace);
	check(shown("Master Level") == "40", "and the parameter shows the step it went to", shown("Master Level"));
	setSteps(40.0f);
	run(100);
	std::snprintf(text, sizeof text, "%.5f against %.5f", level(), high);
	check(std::fabs(level() - high) < high * 0.002f, "the same step set again changes nothing", text);

	// A choice has nothing between its values: set at once.
	const int mode = paramCalled("Synth Mode");
	const std::string before = shown("Synth Mode");
	fx->setParameter(fx, mode, 1.0f);
	run(150);
	check(shown("Synth Mode") != before, "a choice is still set as a whole", before + " -> " + shown("Synth Mode"));

	send(0x80, 84, 0);
	run(100);
	disp(effStopProcess);
	disp(effMainsChanged, 0, 0);
	disp(effClose);
	std::printf("Result: %s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
