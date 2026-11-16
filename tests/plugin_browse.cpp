// A small VST2 host that browses presets the way a person at an MPC does, to shake out crashes
// in loading one preset over another: jumps between folders and presets with random pauses (down
// to none, as a turned Q-Link gives), notes held across the changes, and now and then a few
// engine parameters moved.
//
//   tools/plugintest.sh browse [seed] [steps]
//
// Arguments: <plugin library> [seed, default 1] [steps, default 300] [folder name to stay in]
// With "params:Name,Name,..." in place of the folder it instead moves only those parameters (the
// controls of one screen, say), changing preset every 40 steps:
//   ... 1 400 "params:Arp Mode,Arp Octaves,Arp Rate,Arp Sync,Arp Gate"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_report.h"
#include "mpc_vst2_abi.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

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
static uint32_t rngState = 1;
static uint32_t rnd(uint32_t n) {
	rngState = rngState * 1664525u + 1013904223u;
	return (rngState >> 8) % n;
}

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
	std::exit(1);
}
static std::string shown(int index) {
	char text[64] = {0};
	disp(effGetParamDisplay, index, 0, text);
	return text;
}

static void note(int key, int velocity) {
	struct {
		VstInt32 numEvents;
		VstIntPtr reserved;
		VstEvent* events[2];
	} list = {};
	VstMidiEvent event = {};
	event.type = kVstMidiType;
	event.byteSize = sizeof(VstMidiEvent);
	event.midiData[0] = (char)(velocity != 0 ? 0x90 : 0x80);
	event.midiData[1] = (char)key;
	event.midiData[2] = (char)velocity;
	list.numEvents = 1;
	list.events[0] = reinterpret_cast<VstEvent*>(&event);
	disp(effProcessEvents, 0, 0, &list);
}

static double peak = 0;
static void run(int milliseconds, bool realTime) {
	static float left[128], right[128];
	float* outs[2] = {left, right};
	for (int ms = 0; ms < milliseconds; ms += 3) {
		fx->processReplacing(fx, nullptr, outs, 128);
		for (int i = 0; i < 128; i++) {
			peak = std::fmax(peak, std::fabs(left[i]));
		}
		if (realTime) {
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
		}
	}
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::printf("usage: plugin_browse <plugin library> [seed] [steps] [folder]\n");
		return 1;
	}
	rngState = argc > 2 ? (uint32_t)std::atoi(argv[2]) : 1;
	const int steps = argc > 3 ? std::atoi(argv[3]) : 300;
	const char* stayIn = argc > 4 ? argv[4] : nullptr;
	std::vector<std::string> twiddle;
	if (stayIn != nullptr && std::strncmp(stayIn, "params:", 7) == 0) {
		std::string list = stayIn + 7;
		for (size_t at = 0; at <= list.size();) {
			const size_t comma = std::min(list.find(',', at), list.size());
			twiddle.push_back(list.substr(at, comma - at));
			at = comma + 1;
		}
		stayIn = nullptr;
	}
	crash_report::install();
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
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
	{
		// Where the plugin was loaded, so a crash address inside it can be looked up in its file.
		Dl_info where = {};
		dladdr(reinterpret_cast<void*>(entry), &where);
		std::printf("plugin loaded at %p\n", where.dli_fbase);
	}
	disp(effSetSampleRate, 0, 0, nullptr, 44100.0f);
	disp(effSetBlockSize, 0, 128);
	disp(effOpen);
	disp(effMainsChanged, 0, 1);
	disp(effStartProcess);
	run(300, true);

	const int preset = paramCalled("Preset"), folder = paramCalled("Folder");
	const int presetNext = paramCalled("Preset +"), presetPrev = paramCalled("Preset -");
	const int firstEngineParam = paramCalled("Gold 1");
	int held[4] = {0, 0, 0, 0};

	for (int step = 0; step < steps; step++) {
		const uint32_t what = rnd(10);
		if (!twiddle.empty()) {
			if (step % 40 == 0) {
				// BROWSE_FOLDER=name in the environment keeps the presets to one folder.
				const char* only = std::getenv("BROWSE_FOLDER");
				do {
					fx->setParameter(fx, preset, (float)rnd(1001) / 1000.0f);
				} while (only != nullptr && shown(folder) != only);
				run(400, true);
			}
			const int count = 1 + (int)rnd(3);
			for (int i = 0; i < count; i++) {
				const std::string& name = twiddle[rnd((uint32_t)twiddle.size())];
				const int index = paramCalled(name.c_str());
				// Half the time an extreme, as a switch or a flicked knob gives.
				const float value = rnd(2) != 0 ? (float)rnd(2) : (float)rnd(1001) / 1000.0f;
				fx->setParameter(fx, index, value);
				std::printf("  %s = %.3f\n", name.c_str(), value);
			}
		}
		else if (stayIn != nullptr && shown(folder) != stayIn) {
			for (int i = 0; i <= 200 && shown(folder) != stayIn; i++) {
				fx->setParameter(fx, folder, (float)i / 200.0f);
			}
		}
		else if (what < 2 && stayIn == nullptr) {
			fx->setParameter(fx, folder, (float)rnd(1001) / 1000.0f);
		}
		else if (what < 4) {
			fx->setParameter(fx, preset, (float)rnd(1001) / 1000.0f);
		}
		else if (what < 8) {
			const int button = what < 7 ? presetNext : presetPrev;
			fx->setParameter(fx, button, 1.0f);
			run(3, false);
			fx->setParameter(fx, button, 0.0f);
		}
		else {
			for (int i = 0; i < 4; i++) {
				fx->setParameter(fx, firstEngineParam + (int)rnd((uint32_t)(fx->numParams - firstEngineParam)),
				                 (float)rnd(1001) / 1000.0f);
			}
		}
		std::printf("step %d: %s / %s\n", step, shown(folder).c_str(), shown(preset).c_str());
		const int slot = (int)rnd(4);
		if (held[slot] != 0) {
			note(held[slot], 0);
		}
		held[slot] = 36 + (int)rnd(48);
		note(held[slot], 100);
		// Pauses from nothing (the next change lands while this one is loading) to long enough for
		// the load to finish and the note to sound.
		static const int pauses[] = {0, 0, 6, 20, 60, 150, 400};
		run(pauses[rnd(7)], true);
	}
	run(500, true);
	disp(effStopProcess);
	disp(effMainsChanged, 0, 0);
	disp(effClose);
	std::printf("Result: PASS (%d steps, peak %.3f)\n", steps, peak);
	return 0;
}
