// A small VST2 host that loads the built Deluge Synth plugin and checks how its "engine
// parameters" (the gold knobs and the shortcut-grid pads) behave together, which the SDK's
// host_sim does not look at:
//
//   - a gold knob and the pad that reach the same thing follow each other
//   - a pad that does nothing in the preset reads "-" until another pad brings it to life
//   - a gold knob on a modulation depth creates its patch cable
//   - a value set again and again is not pulled back by the read-back
//   - a saved state comes back whole, including pads that only apply once another is set, both
//     over a different preset and over the same one
//
//   tools/plugintest.sh           (builds this and runs it against the native plugin build)
//
// Arguments: <plugin library> [preset name, default SYNT000]
// Needs the preset in the plugin's Content/SYNTHS and, in it, gold knob 3 on LPF frequency and
// gold knob 12 on LFO1 -> pitch, as in the factory SYNT000.
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

static std::string shown(int index) {
	char text[64] = {0};
	disp(effGetParamDisplay, index, 0, text);
	return text;
}
static std::string shown(const char* name) { return shown(paramCalled(name)); }

// Range of a parameter by name: the grid table's, or a gold knob's.
static void rangeOf(const char* name, int* lo, int* hi) {
	*lo = -64;
	*hi = 64;
	for (const grid::Cell& cell : grid::kCells) {
		if (std::strcmp(cell.name, name) == 0) {
			*lo = cell.min;
			*hi = cell.max;
		}
	}
}
static void setValue(const char* name, int value) {
	int lo, hi;
	rangeOf(name, &lo, &hi);
	fx->setParameter(fx, paramCalled(name), (float)(value - lo) / (float)(hi - lo));
}
static int valueOf(const char* name) {
	int lo, hi;
	rangeOf(name, &lo, &hi);
	return lo + (int)std::lround(fx->getParameter(fx, paramCalled(name)) * (float)(hi - lo));
}

// Audio for a while, in real time enough for the plugin's worker thread to do its part.
static void run(int milliseconds = 120) {
	static float left[128], right[128];
	float* outs[2] = {left, right};
	for (int ms = 0; ms < milliseconds; ms += 3) {
		fx->processReplacing(fx, nullptr, outs, 128);
		std::this_thread::sleep_for(std::chrono::milliseconds(3));
	}
}

static void check(bool ok, const char* what, const std::string& detail = "") {
	std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
	if (!ok) {
		failures++;
	}
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::printf("usage: plugin_grid <plugin library> [preset]\n");
		return 1;
	}
	const char* presetName = argc > 2 ? argv[2] : "SYNT000";
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
	run(200);

	// Find the preset by walking the Preset parameter. Its range is the number of presets, so
	// step through normalised values finely.
	bool found = false;
	{
		const int preset = paramCalled("Preset");
		for (int i = 0; i <= 2000 && !found; i++) {
			fx->setParameter(fx, preset, (float)i / 2000.0f);
			found = shown(preset) == presetName;
		}
		run(500);
	}
	check(found, "preset found", presetName);
	if (!found) {
		return 1;
	}
	const std::string otherPreset = "SYNT008";

	// --- what the preset brought ---
	check(shown("Synth Mode") == "Subtractive", "the preset's synth mode shows", shown("Synth Mode"));
	check(shown("Mod1 Level") == "-", "an FM pad reads - in a subtractive preset", shown("Mod1 Level"));
	check(shown("LPF Freq") != "-" && shown("LPF Freq") != "0", "LPF frequency shows the preset's value", shown("LPF Freq"));

	// --- a gold knob and its pad follow each other ---
	const int padBefore = valueOf("LPF Freq");
	setValue("Gold 3", -30);
	run();
	const int padAfter = valueOf("LPF Freq");
	check(padAfter != padBefore, "moving gold knob 3 moves the LPF frequency pad",
	      std::to_string(padBefore) + " -> " + std::to_string(padAfter));
	check(valueOf("Gold 3") == -30, "and the knob stays where it was put", std::to_string(valueOf("Gold 3")));
	setValue("LPF Freq", 40);
	run();
	check(valueOf("LPF Freq") == 40, "the pad stays where it was put", std::to_string(valueOf("LPF Freq")));
	check(valueOf("Gold 3") > 20, "moving the pad moves gold knob 3", std::to_string(valueOf("Gold 3")));

	// --- a gold knob on a modulation depth ---
	check(valueOf("Gold 12") == -64 && shown("Vibrato") == "0.00", "no vibrato cable yet: knob at its lowest, pad 0.00",
	      std::to_string(valueOf("Gold 12")) + ", " + shown("Vibrato"));
	setValue("Gold 12", 0);
	run();
	check(shown("Vibrato") == "25.00", "gold knob 12 to the middle makes the cable: vibrato pad 25.00", shown("Vibrato"));
	setValue("Vibrato", 0);
	run();
	check(valueOf("Gold 12") == -64, "vibrato pad to 0 removes it: knob back at its lowest", std::to_string(valueOf("Gold 12")));

	// --- a transpose is two parameters, semitones and cents, and one number in the engine ---
	{
		const int semitones = paramCalled("Osc1 Transp"), cents = paramCalled("Osc1 Fine");
		fx->setParameter(fx, semitones, (float)(96 + 7) / 192.0f);
		run();
		fx->setParameter(fx, cents, (float)(50 + 12) / 99.0f);
		run();
		check(shown(semitones) == "+7" && shown(cents) == "+12", "transpose +7 semitones, then +12 cents: both stand",
		      shown(semitones) + ", " + shown(cents));
		fx->setParameter(fx, semitones, (float)(96 - 12) / 192.0f);
		run();
		check(shown(semitones) == "-12" && shown(cents) == "+12", "semitones to -12 keeps the cents", shown(semitones) + ", " + shown(cents));
		fx->setParameter(fx, cents, 0.0f);
		run();
		check(shown(semitones) == "-12" && shown(cents) == "-50", "cents to -50 keeps the semitones", shown(semitones) + ", " + shown(cents));
		fx->setParameter(fx, semitones, 96.0f / 192.0f);
		fx->setParameter(fx, cents, 50.0f / 99.0f);
		run();
		check(shown(semitones) == "0" && shown(cents) == "0", "and back to nothing", shown(semitones) + ", " + shown(cents));
	}

	// --- one pad brings others to life ---
	setValue("Synth Mode", 1);
	run();
	check(shown("Synth Mode") == "Fm" && shown("Mod1 Level") != "-" && shown("Noise") == "-",
	      "synth mode FM: the modulator pad applies, the noise pad does not", shown("Mod1 Level") + ", " + shown("Noise"));

	// --- a value set over and over is not pulled back ---
	bool steady = true;
	for (int v = 0; v <= 50; v++) {
		setValue("Mod1 Level", v);
		run(9);
		steady = steady && valueOf("Mod1 Level") == v;
	}
	run();
	check(steady && valueOf("Mod1 Level") == 50, "a pad turned step by step never jumps back", std::to_string(valueOf("Mod1 Level")));

	// --- save, and restore over another preset and over this one ---
	setValue("Mod1 Level", 37);
	setValue("Osc1 Feedbk", 21);
	setValue("Env1 Attack", 9);
	run();
	void* chunk = nullptr;
	const VstIntPtr size = disp(effGetChunk, 0, 0, &chunk);
	const std::vector<uint8_t> saved(static_cast<uint8_t*>(chunk), static_cast<uint8_t*>(chunk) + size);
	auto restoredAs = [&](const char* what) {
		// Make the engine read everything back, as any later change would: values the engine does
		// not really hold would be replaced by what it does hold.
		setValue("Pan", 3);
		run();
		setValue("Pan", 0);
		run();
		const std::string got = shown("Synth Mode") + ", mod1 " + shown("Mod1 Level") + ", feedback " + shown("Osc1 Feedbk")
		                        + ", attack " + shown("Env1 Attack") + ", preset " + shown("Preset");
		check(shown("Synth Mode") == "Fm" && valueOf("Mod1 Level") == 37 && valueOf("Osc1 Feedbk") == 21
		          && valueOf("Env1 Attack") == 9 && shown("Preset") == presetName,
		      what, got);
	};

	found = false;
	{
		const int preset = paramCalled("Preset");
		for (int i = 0; i <= 2000 && !found; i++) {
			fx->setParameter(fx, preset, (float)i / 2000.0f);
			found = shown(preset) == otherPreset;
		}
		run(500);
	}
	check(found && shown("Env1 Attack") != "9", "another preset brings its own values", shown("Preset") + ", attack " + shown("Env1 Attack"));
	disp(effSetChunk, 0, (VstIntPtr)saved.size(), const_cast<uint8_t*>(saved.data()));
	run(500);
	restoredAs("state restored over a different preset");

	setValue("Synth Mode", 0);
	setValue("Env1 Attack", 30);
	run();
	check(shown("Mod1 Level") == "-", "back to subtractive: the modulator pad reads - again", shown("Mod1 Level"));
	disp(effSetChunk, 0, (VstIntPtr)saved.size(), const_cast<uint8_t*>(saved.data()));
	run(500);
	restoredAs("state restored over the same preset, changed");

	disp(effStopProcess);
	disp(effMainsChanged, 0, 0);
	disp(effClose);
	std::printf("Result: %s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
