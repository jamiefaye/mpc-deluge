// A small VST2 host that runs several instances of the built Deluge Synth plugin in one process,
// each on a thread of its own, as an MPC project with the plugin on several tracks does:
//
//   - each instance sounds, with its own preset
//   - an instance with no notes stays silent while another plays (nothing leaks across)
//   - both play at once from two threads calling process() at the same time
//   - one changes preset again and again while the other holds a chord: the other keeps sounding
//   - an instance is closed while the other plays, and a new one opened
//
//   tools/plugintest.sh two      (or build for arm32 and run under tools/armtest/run.py --exe)
//
// Arguments: <plugin library>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_report.h"
#include "mpc_vst2_abi.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>

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

using Entry = AEffect* (*)(audioMasterCallback);
static Entry entry;
static int failures = 0;

static void check(bool ok, const char* what, const std::string& detail = "") {
	std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
	if (!ok) {
		failures++;
	}
}

// One plugin instance and the thread that plays it.
struct Track {
	AEffect* fx = nullptr;
	std::thread thread;
	std::atomic<bool> running{false};
	std::atomic<int> noteOn{-1}, noteOff{-1}; // handed to the audio thread
	std::atomic<float> peak{0.0f};            // since last asked
	std::atomic<int> blocks{0}, silentBlocks{0};

	VstIntPtr disp(VstInt32 opcode, VstInt32 index = 0, VstIntPtr value = 0, void* ptr = nullptr, float opt = 0.0f) {
		return fx->dispatcher(fx, opcode, index, value, ptr, opt);
	}
	int param(const char* name) {
		for (int i = 0; i < fx->numParams; i++) {
			char text[64] = {0};
			disp(effGetParamName, i, 0, text);
			if (std::strcmp(text, name) == 0) {
				return i;
			}
		}
		return 0;
	}
	std::string shown(const char* name) {
		char text[64] = {0};
		disp(effGetParamDisplay, param(name), 0, text);
		return text;
	}
	bool open() {
		fx = entry(hostCallback);
		if (fx == nullptr) {
			return false;
		}
		disp(effSetSampleRate, 0, 0, nullptr, 44100.0f);
		disp(effSetBlockSize, 0, 128);
		disp(effOpen);
		disp(effMainsChanged, 0, 1);
		disp(effStartProcess);
		running.store(true);
		thread = std::thread([this] { audio(); });
		return true;
	}
	void close() {
		running.store(false);
		thread.join();
		disp(effStopProcess);
		disp(effMainsChanged, 0, 0);
		disp(effClose);
		fx = nullptr;
	}
	void send(int status, int key, int velocity) {
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
	void audio() {
		float left[128], right[128];
		float* outs[2] = {left, right};
		while (running.load()) {
			int key = noteOn.exchange(-1);
			if (key >= 0) {
				send(0x90, key, 100);
			}
			key = noteOff.exchange(-1);
			if (key >= 0) {
				send(0x80, key, 0);
			}
			fx->processReplacing(fx, nullptr, outs, 128);
			float blockPeak = 0;
			for (int i = 0; i < 128; i++) {
				blockPeak = std::fmax(blockPeak, std::fabs(left[i]));
			}
			if (blockPeak > peak.load()) {
				peak.store(blockPeak);
			}
			blocks.fetch_add(1);
			if (blockPeak == 0.0f) {
				silentBlocks.fetch_add(1);
			}
			std::this_thread::sleep_for(std::chrono::microseconds(2900));
		}
	}
	// Chooses a preset by name through the Preset parameter, as a host does.
	bool choose(const char* name) {
		// Every folder, and in each every position of Preset (the position within the folder).
		const int folder = param("Folder"), preset = param("Preset");
		for (int f = 0; f <= 40; f++) {
			fx->setParameter(fx, folder, (float)f / 40.0f);
			for (int i = 0; i <= 400; i++) {
				fx->setParameter(fx, preset, (float)i / 400.0f);
				char text[64] = {0};
				disp(effGetParamDisplay, preset, 0, text);
				if (std::strcmp(text, name) == 0) {
					return true;
				}
			}
		}
		return false;
	}
	float takePeak() { return peak.exchange(0.0f); }
};

static void wait(int milliseconds) {
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

int main(int argc, char** argv) {
	crash_report::install();
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	if (argc < 2) {
		std::printf("usage: plugin_two <plugin library>\n");
		return 1;
	}
	void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	entry = library != nullptr ? reinterpret_cast<Entry>(dlsym(library, "VSTPluginMain")) : nullptr;
	if (entry == nullptr) {
		std::printf("FAIL  cannot load %s\n", argv[1]);
		return 1;
	}
	{
		Dl_info where = {};
		dladdr(reinterpret_cast<void*>(entry), &where);
		std::printf("plugin loaded at %p\n", where.dli_fbase);
	}
	// The emulated machine is slow; everything waits longer there.
	const int slow = std::getenv("PLUGIN_TWO_SLOW") != nullptr ? 4 : 1;

	static Track a, b;
	check(a.open() && b.open(), "two instances open");
	wait(600 * slow);
	check(a.choose("SYNT000") && b.choose("SYNT008"), "each takes a preset of its own", a.shown("Preset") + ", " + b.shown("Preset"));
	wait(800 * slow);
	a.takePeak();
	b.takePeak();

	// --- one plays, the other must stay silent ---
	a.noteOn.store(48);
	wait(500 * slow);
	const float aAlone = a.takePeak(), bIdle = b.takePeak();
	check(aAlone > 0.01f && bIdle < 0.0001f, "the first plays and nothing of it reaches the second (only its own dither)",
	      std::to_string(aAlone) + ", " + std::to_string(bIdle));
	a.noteOff.store(48);
	wait(1500 * slow); // release and reverb tail
	a.takePeak();
	b.noteOn.store(60);
	wait(500 * slow);
	const float bAlone = b.takePeak(), aIdle = a.takePeak();
	check(bAlone > 0.01f && aIdle < 0.001f, "the second plays and (but for the first's dying tail) the first is quiet",
	      std::to_string(bAlone) + ", " + std::to_string(aIdle));

	// --- both at once ---
	a.noteOn.store(48);
	wait(500 * slow);
	const float aBoth = a.takePeak(), bBoth = b.takePeak();
	check(aBoth > 0.01f && bBoth > 0.01f, "both play at once", std::to_string(aBoth) + ", " + std::to_string(bBoth));
	check(a.shown("Synth Mode") != b.shown("Synth Mode"), "each shows its own preset's values",
	      a.shown("Synth Mode") + ", " + b.shown("Synth Mode"));

	// --- parameters are each instance's own ---
	const int cutoff = a.param("LPF Freq");
	const float bBefore = b.fx->getParameter(b.fx, cutoff);
	a.fx->setParameter(a.fx, cutoff, 0.1f);
	wait(300 * slow);
	check(std::fabs(a.fx->getParameter(a.fx, cutoff) - 0.1f) < 0.02f && b.fx->getParameter(b.fx, cutoff) == bBefore,
	      "a pad moved in one does not move in the other");

	// --- one browses presets while the other holds its note ---
	b.noteOff.store(60);
	a.blocks.store(0);
	a.silentBlocks.store(0);
	static const char* const names[] = {"SYNT001", "SYNT005", "SYNT008", "SYNT000"};
	for (int i = 0; i < 12; i++) {
		b.choose(names[i % 4]);
		b.noteOn.store(55 + i);
		wait(250 * slow);
		b.noteOff.store(55 + i);
	}
	const int blocks = a.blocks.load(), silent = a.silentBlocks.load();
	const float aDuring = a.takePeak();
	check(aDuring > 0.01f && silent * 2 < blocks, "the first keeps sounding while the second changes preset twelve times",
	      std::to_string(silent) + " of " + std::to_string(blocks) + " blocks silent (a preset load is a gap in every instance)");

	// --- close one, open another ---
	b.close();
	wait(300 * slow);
	a.takePeak();
	wait(300 * slow);
	check(a.takePeak() > 0.01f, "the first still plays after the second is closed");
	static Track c;
	check(c.open(), "a third opens in the freed place");
	wait(600 * slow);
	c.choose("SYNT005");
	wait(800 * slow);
	c.takePeak();
	c.noteOn.store(64);
	wait(500 * slow);
	check(c.takePeak() > 0.01f && a.takePeak() > 0.01f, "and plays beside the first");

	a.close();
	c.close();
	std::printf("Result: %s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
	return failures == 0 ? 0 : 1;
}
