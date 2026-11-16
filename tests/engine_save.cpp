// A sound saved with engineSaveSynth() and loaded again must be the sound that was saved.
//
// For every Nth preset in a folder: load it, move some gold knobs and pads so that it is not the
// file's own state, play the test phrase and keep a checksum; save it; load the saved file into
// the same instance and play again. The two takes must match sample for sample (run with
// DELUGE_SCRUB=165, see CLAUDE.md "Comparing builds"), or differ only faintly.
//
//   tools/build_firmware.py native --test tests/engine_save.cpp
//   DELUGE_SCRUB=165 build/firmware/native/engine_save build/corpus/root SYNTHS 7
//
// Arguments: <root folder, which is written to> [folder under it, default SYNTHS] [every Nth, default 7]
// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_report.h"
#include "engine.h"
#include "matrix.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace deluge_hosted;

static std::vector<int32_t> play() {
	std::vector<int32_t> audio;
	int32_t buffer[kMaxRenderFrames * 2];
	engineNoteOn(48, 100);
	engineNoteOn(60, 80);
	engineNoteOn(67, 110);
	for (int block = 0; block < 120; block++) {
		if (block == 80) {
			engineNoteOff(48);
			engineNoteOff(60);
			engineNoteOff(67);
		}
		engineRender(buffer, kMaxRenderFrames);
		audio.insert(audio.end(), buffer, buffer + kMaxRenderFrames * 2);
	}
	return audio;
}

// Changes that a save has to carry: gold knobs, and a few pads of different kinds.
static void edit() {
	// ENGINE_SAVE_EDIT narrows it down: "k3" only gold knob 3, "p2" only the third pad below.
	const char* only = std::getenv("ENGINE_SAVE_EDIT");
	for (int knob = 0; knob < kNumKnobs; knob++) {
		if (only != nullptr && !(only[0] == 'k' && std::atoi(only + 1) == knob)) {
			continue;
		}
		int position = 0;
		if (engineKnobGet(knob, &position)) {
			engineKnobSet(knob, position + (knob % 2 ? -9 : 9));
		}
	}
	static const int pads[][3] = {{0, 8, 7}, {0, 8, 4}, {0, 2, 1}, {0, 6, 3}, {0, 9, 6}}; // LPF freq, env attack, osc1 transpose, pan, HPF reso
	for (const auto& pad : pads) {
		if (only != nullptr && !(only[0] == 'p' && std::atoi(only + 1) == (int)(&pad - pads))) {
			continue;
		}
		CellInfo info;
		if (engineCellInfo(pad[0], pad[1], pad[2], &info) && info.relevant && info.kind == CellKind::Number) {
			engineCellSet(pad[0], pad[1], pad[2], info.min + (info.max - info.min) / 3);
		}
	}
}

int main(int argc, char** argv) {
	crash_report::install();
	const char* root = argc > 1 ? argv[1] : "/sd";
	const char* folder = argc > 2 ? argv[2] : "SYNTHS";
	const int every = argc > 3 ? std::max(1, std::atoi(argv[3])) : 7;
	setvbuf(stdout, nullptr, _IONBF, 0);
	std::vector<std::string> presets;
	const std::string dirPath = std::string(root) + "/" + folder;
	if (DIR* d = opendir(dirPath.c_str())) {
		while (dirent* e = readdir(d)) {
			const size_t n = std::strlen(e->d_name);
			if (n > 4 && strcasecmp(e->d_name + n - 4, ".xml") == 0 && std::strncmp(e->d_name, "ENGINE-SAVE", 11) != 0) {
				presets.push_back(std::string(folder) + "/" + e->d_name);
			}
		}
		closedir(d);
	}
	std::sort(presets.begin(), presets.end());
	if (const char* only = std::getenv("ENGINE_SAVE_ONLY")) { // one preset, by file name
		presets.assign(1, std::string(folder) + "/" + only);
	}
	const std::string saved = std::string(folder) + "/ENGINE-SAVE-TEST.XML";
	const std::string savedOnDisk = std::string(root) + "/" + saved;

	engineInit(root);
	int same = 0, faint = 0, different = 0, failed = 0, tried = 0;
	for (size_t i = 0; i < presets.size(); i += (size_t)every) {
		alarm(120);
		tried++;
		unlink(savedOnDisk.c_str());
		// Each take in a new instance, so that both start from the same noise generator and
		// clock (the ladder filters and free-running oscillators draw on them).
		int instance = engineNewInstance();
		if (engineLoadSynth(presets[i].c_str()) != 0) {
			std::printf("PRESET %s: would not load\n", presets[i].c_str());
			failed++;
			engineDeleteInstance(instance);
			continue;
		}
		if (std::getenv("ENGINE_SAVE_NO_EDIT") == nullptr) { // set it to save the preset as loaded
			edit();
		}
		const int saveError = engineSaveSynth(saved.c_str());
		const std::vector<int32_t> before = play();
		if (saveError != 0) {
			std::printf("PRESET %s: save error %d\n", presets[i].c_str(), saveError);
			failed++;
			engineDeleteInstance(instance);
			continue;
		}
		if (engineSaveSynth(saved.c_str()) == 0) {
			std::printf("PRESET %s: a second save overwrote the first\n", presets[i].c_str());
			failed++;
		}
		engineDeleteInstance(instance);
		instance = engineNewInstance();
		const int loadError = engineLoadSynth(saved.c_str());
		if (loadError != 0) {
			std::printf("PRESET %s: the saved file would not load, error %d\n", presets[i].c_str(), loadError);
			failed++;
			engineDeleteInstance(instance);
			continue;
		}
		const std::vector<int32_t> after = play();
		engineDeleteInstance(instance);
		int64_t largest = 0;
		size_t count = 0;
		for (size_t k = 0; k < before.size(); k++) {
			const int64_t d = (int64_t)before[k] - (int64_t)after[k];
			if (d != 0) {
				count++;
				largest = std::max<int64_t>(largest, d < 0 ? -d : d);
			}
		}
		if (largest == 0) {
			same++;
		}
		else {
			const double down = -20.0 * std::log10((double)largest / 2147483648.0);
			(down > 90.0 ? faint : different)++;
			std::printf("PRESET %s: %s after save and load (%d samples, largest %.1f dB below full scale)\n", presets[i].c_str(),
			            down > 90.0 ? "faintly different" : "DIFFERENT", (int)count, down);
		}
	}
	if (std::getenv("ENGINE_SAVE_KEEP") == nullptr) {
		unlink(savedOnDisk.c_str());
	}
	std::printf("SAVE %d presets: %d the same after save and load, %d faintly different, %d different, %d failed\n", tried, same,
	            faint, different, failed);
	return (different || failed) ? 1 : 0;
}
