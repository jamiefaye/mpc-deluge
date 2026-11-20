// A small VST2 host that loads the built Deluge Synth plugin and checks variations: a sound
// that arrives in a project's state with more than one set of parameter values (the "X" block of
// saveExtraState in DelugeSynth.cpp, which is what the Deluge song converter writes), and the
// Variation parameter that chooses between them.
//
//   - a state made by hand, as a converter makes one, loads: the sound's name shows in the
//     preset's place and the parameters show the first variation's values
//   - Variation changes which values play and show, and back; a number with no variation
//     behind it changes nothing; it is not an edit (no star)
//   - a pad changed in one variation is changed in that one only
//   - a project saved from that comes back with both variations, the edit, and the one in use
//   - another preset is itself again, and Variation does nothing to it
//
//   tools/plugintest.sh           (builds this and runs it against the native plugin build)
//
// Arguments: <plugin library> [preset name, default SYNT000]
// Needs the preset and SYNT008 in the plugin's Content/SYNTHS.
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

// Chooses a preset by name as a host can: every folder, and in each every position of Preset
// (which is the position within the folder).
static bool choosePreset(const char* name) {
	const int folder = paramCalled("Folder"), preset = paramCalled("Preset");
	for (int f = 0; f <= 40; f++) {
		fx->setParameter(fx, folder, (float)f / 40.0f);
		for (int i = 0; i <= 400; i++) {
			fx->setParameter(fx, preset, (float)i / 400.0f);
			// (a preset with an edit kept from earlier in the session shows a star after its name)
			if (shown(preset) == name || shown(preset) == std::string(name) + " *") {
				return true;
			}
		}
	}
	return false;
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


// Presses Save and returns the text of the preset file it wrote (which is then removed): the
// sound as it stands, written by the firmware's writer.
static std::string savedText(const std::string& content, const char* presetName) {
	const std::string file = content + "/Content/SYNTHS/User/" + presetName + " 2.XML";
	std::remove(file.c_str());
	fx->setParameter(fx, paramCalled("Save"), 1.0f);
	run(1200);
	std::string text;
	if (FILE* f = std::fopen(file.c_str(), "rb")) {
		char buffer[4096];
		for (size_t got; (got = std::fread(buffer, 1, sizeof buffer, f)) != 0;) {
			text.append(buffer, got);
		}
		std::fclose(f);
	}
	std::remove(file.c_str());
	return text;
}

static void append32(std::vector<uint8_t>& to, uint32_t value) {
	to.insert(to.end(), reinterpret_cast<const uint8_t*>(&value), reinterpret_cast<const uint8_t*>(&value) + 4);
}

static void setVariation(int n) {
	fx->setParameter(fx, paramCalled("Variation"), (float)n / 31.0f);
	run(300);
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::printf("usage: plugin_variations <plugin library> [preset]\n");
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
	// The native build's Content/ is the project's own: <library>/../../Content.
	std::string content = argv[1];
	for (int up = 0; up < 3; up++) {
		content.resize(content.rfind('/'));
	}

	// Two texts: the preset as the engine writes it, and the same with three pads moved.
	if (!choosePreset(presetName)) {
		std::printf("FAIL  no preset %s\n", presetName);
		return 1;
	}
	run(500);
	const int freq0 = valueOf("LPF Freq"), attack0 = valueOf("Env1 Attack"), reso0 = valueOf("LPF Reso");
	const std::string first = savedText(content, presetName);
	const int freq1 = freq0 == 40 ? 30 : 40, attack1 = attack0 == 9 ? 12 : 9, reso1 = reso0 == 33 ? 20 : 33;
	setValue("LPF Freq", freq1);
	setValue("Env1 Attack", attack1);
	run();
	const std::string second = savedText(content, presetName);
	check(first.find("<sound") != std::string::npos && second.find("<sound") != std::string::npos && first != second,
	      "two preset texts to build a state from", std::to_string(first.size()) + " and " + std::to_string(second.size()) + " bytes");

	// A state as the converter makes one: the SDK framework's block with no parameters in it,
	// and ours with no preset path, no flags, and the "X" block.
	std::vector<uint8_t> extra{0, 0, 'X', 1, 0, 2}; // version 1, kind 0 (a synth), two variations
	const char* name = "Converted Lead";
	extra.insert(extra.end(), name, name + std::strlen(name) + 1);
	for (const std::string* text : {&first, &second}) {
		append32(extra, (uint32_t)text->size());
		extra.insert(extra.end(), text->begin(), text->end());
	}
	std::vector<uint8_t> state{'M', 'P', 'C', 'P'};
	append32(state, 2);
	append32(state, 0);
	append32(state, (uint32_t)extra.size());
	state.insert(state.end(), extra.begin(), extra.end());

	// It arrives while another preset is loaded, and plays in that one's place.
	check(choosePreset("SYNT008"), "another preset to load the state over");
	run(500);
	disp(effSetChunk, 0, (VstIntPtr)state.size(), state.data());
	run(300);
	auto values = [&] {
		return "preset " + shown("Preset") + ", variation " + shown("Variation") + ", LPF " + std::to_string(valueOf("LPF Freq")) + ", attack "
		       + std::to_string(valueOf("Env1 Attack")) + ", resonance " + std::to_string(valueOf("LPF Reso"));
	};
	check(shown("Preset") == name && shown("Variation") == "1" && valueOf("LPF Freq") == freq0 && valueOf("Env1 Attack") == attack0,
	      "a state with two variations loads: the sound's name, and the first one's values", values());
	setVariation(1);
	check(shown("Variation") == "2" && valueOf("LPF Freq") == freq1 && valueOf("Env1 Attack") == attack1 && shown("Preset") == name,
	      "Variation 2: the second one's values show, and the name has no star", values());
	setVariation(0);
	check(valueOf("LPF Freq") == freq0 && valueOf("Env1 Attack") == attack0, "Variation 1 again: the first one's values", values());
	setVariation(1);
	setValue("LPF Reso", reso1);
	run();
	check(valueOf("LPF Reso") == reso1 && valueOf("LPF Freq") == freq1 && shown("Preset") == std::string(name) + " *",
	      "a pad changed in variation 2 stays, and stars the name", values());
	setVariation(0);
	check(valueOf("LPF Reso") == reso0 && valueOf("LPF Freq") == freq0, "variation 1 does not have that change", values());
	setVariation(7);
	check(shown("Variation") == "8" && valueOf("LPF Reso") == reso0 && valueOf("LPF Freq") == freq0,
	      "a Variation the sound does not have changes nothing", values());
	setVariation(1);
	check(valueOf("LPF Reso") == reso1 && valueOf("LPF Freq") == freq1, "variation 2 again: its change is there", values());

	// A project saved now holds both, the change, and which one plays.
	void* chunk = nullptr;
	const VstIntPtr size = disp(effGetChunk, 0, 0, &chunk);
	const std::vector<uint8_t> saved(static_cast<uint8_t*>(chunk), static_cast<uint8_t*>(chunk) + size);
	int sounds = 0;
	for (size_t at = 0; at + 6 < saved.size(); at++) {
		sounds += std::memcmp(saved.data() + at, "<sound", 6) == 0 ? 1 : 0;
	}
	check(sounds == 2, "a project save holds both variations as preset texts", std::to_string(saved.size()) + " bytes, " + std::to_string(sounds) + " sounds");

	// Another preset is itself, whatever Variation says.
	check(choosePreset(presetName), "the first preset again");
	run(500);
	check(shown("Preset") == presetName && valueOf("LPF Freq") == freq0 && valueOf("Env1 Attack") == attack0,
	      "a preset of the library is itself while Variation says 2", values());
	setVariation(0);
	check(valueOf("LPF Freq") == freq0, "and Variation does nothing to it", values());

	disp(effSetChunk, 0, (VstIntPtr)saved.size(), const_cast<uint8_t*>(saved.data()));
	run(300);
	check(shown("Preset") == name && shown("Variation") == "2" && valueOf("LPF Freq") == freq1 && valueOf("LPF Reso") == reso1,
	      "the saved project comes back in variation 2, with the change made there", values());
	setVariation(0);
	check(valueOf("LPF Freq") == freq0 && valueOf("LPF Reso") == reso0 && valueOf("Env1 Attack") == attack0, "and its variation 1 as it was", values());

	// A state cut short is refused whole, and the preset named by its path, or the one selected, is loaded as its file has it.
	std::vector<uint8_t> cut(state.begin(), state.end() - 500);
	const uint32_t cutExtra = (uint32_t)(extra.size() - 500);
	std::memcpy(cut.data() + 12, &cutExtra, 4);
	disp(effSetChunk, 0, (VstIntPtr)cut.size(), cut.data());
	run(300);
	check(shown("Preset") == "SYNT008" && shown("Variation") == "1", "a state cut short is not loaded in part: the preset it would have replaced plays", values());

	// So is a kind of sound this build does not know (the byte after the version).
	disp(effSetChunk, 0, (VstIntPtr)saved.size(), const_cast<uint8_t*>(saved.data()));
	run(300);
	std::vector<uint8_t> otherKind = state;
	otherKind[16 + 4] = 1;
	disp(effSetChunk, 0, (VstIntPtr)otherKind.size(), otherKind.data());
	run(300);
	check(shown("Preset") == "SYNT008", "a state holding another kind of sound is refused, and the preset plays", values());

	disp(effStopProcess);
	disp(effMainsChanged, 0, 0);
	disp(effClose);
	std::printf("Result: %s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
