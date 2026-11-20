// Variations (engine.h): one sound, several sets of parameter values, cables and arpeggiator
// settings, as the clips of one synth have on a Deluge.
//
// For every Nth preset in a folder: an edited copy of it is saved as a second preset file. Then,
// each take in a new instance so that all start from the same clock and noise:
//   A  the preset alone                  B  the edited file alone
//   C  the preset with the file added as variation 1, variation 0 in use        must be A
//   D  the same with variation 1 in use                                           must be B
//   E  the same, variation 1 and then 0 again                                     must be A
//   F  variation 1 saved from D's instance while 0 is in use, loaded alone        must be B
// (D and F only where the edited file plays the same twice alone: a few sounds do not)
// and a run that changes variation every block while notes sound, which must not stop the
// firmware at one of its checks. Run with DELUGE_SCRUB=165 (CLAUDE.md, "Comparing builds").
//
//   tools/build_firmware.py native --test tests/engine_variations.cpp
//   DELUGE_SCRUB=165 build/firmware/native/engine_variations build/corpus/root SYNTHS 7
//
// Arguments: <root folder, which is written to> [folder under it, default SYNTHS] [every Nth, default 7]
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cables.h"
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
#include <strings.h>
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

// What a clip holds and another clip of the same synth may hold differently: parameter values
// (through gold knobs and pads), a cable's amount, a cable of its own.
static void edit() {
	// ENGINE_VAR_EDIT narrows it down: "k3" only gold knob 3, "p2" only the third pad below, "c" only the cables.
	const char* only = std::getenv("ENGINE_VAR_EDIT");
	for (int knob = 0; knob < kNumKnobs; knob++) {
		if (only != nullptr && !(only[0] == 'k' && std::atoi(only + 1) == knob)) {
			continue;
		}
		int position = 0;
		if (engineKnobGet(knob, &position)) {
			engineKnobSet(knob, position + (knob % 2 ? -9 : 9));
		}
	}
	static const int pads[][3] = {{0, 8, 7}, {0, 8, 4}, {0, 6, 3}, {0, 9, 6}}; // LPF freq, env attack, pan, HPF reso
	for (const auto& pad : pads) {
		if (only != nullptr && !(only[0] == 'p' && std::atoi(only + 1) == (int)(&pad - pads))) {
			continue;
		}
		CellInfo info;
		if (engineCellInfo(pad[0], pad[1], pad[2], &info) && info.relevant && info.kind == CellKind::Number) {
			engineCellSet(pad[0], pad[1], pad[2], info.min + (info.max - info.min) / 3);
		}
	}
	if (only != nullptr && only[0] != 'c') {
		return;
	}
	Cable list[kMaxCables];
	const int count = std::min(engineCableList(list, kMaxCables), (int)kMaxCables);
	if (count > 0) {
		engineCableSet(list[0].source, list[0].destination, list[0].of, list[0].amount > 0 ? -1500 : 1500);
	}
	if (count < kMaxCables - 1) {
		engineCableSet(list[count > 1 ? 1 : 0].source >= 0 && count > 0 ? list[0].source : 0, 2, -1, 700);
	}
}

// How far two takes are apart: 0 the same, else dB below full scale of the largest difference.
static double apart(const std::vector<int32_t>& a, const std::vector<int32_t>& b) {
	int64_t largest = a.size() == b.size() ? 0 : INT32_MAX;
	for (size_t k = 0; k < a.size() && k < b.size(); k++) {
		const int64_t d = (int64_t)a[k] - (int64_t)b[k];
		largest = std::max<int64_t>(largest, d < 0 ? -d : d);
	}
	return largest == 0 ? 0.0 : -20.0 * std::log10((double)largest / 2147483648.0);
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
			if (n > 4 && strcasecmp(e->d_name + n - 4, ".xml") == 0 && std::strncmp(e->d_name, "ENGINE-VAR", 10) != 0) {
				presets.push_back(std::string(folder) + "/" + e->d_name);
			}
		}
		closedir(d);
	}
	std::sort(presets.begin(), presets.end());
	if (const char* only = std::getenv("ENGINE_VAR_ONLY")) { // one preset, by file name
		presets.assign(1, std::string(folder) + "/" + only);
	}
	const std::string edited = std::string(folder) + "/ENGINE-VAR-EDIT.XML", resaved = std::string(folder) + "/ENGINE-VAR-SAVED.XML";
	const std::string editedOnDisk = std::string(root) + "/" + edited, resavedOnDisk = std::string(root) + "/" + resaved;

	engineInit(root);
	int good = 0, faint = 0, bad = 0, failed = 0, tried = 0, noEffect = 0, unrepeatable = 0;
	char firmwareError[8] = {0};
	const int errorsAtStart = engineFirmwareErrors(firmwareError, sizeof firmwareError);
	for (size_t i = 0; i < presets.size(); i += (size_t)every) {
		alarm(180);
		tried++;
		unlink(editedOnDisk.c_str());
		unlink(resavedOnDisk.c_str());
		const char* preset = presets[i].c_str();
		// A: the preset alone. Its edited copy is written from the same instance, after the take.
		int instance = engineNewInstance();
		if (engineLoadSynth(preset) != 0) {
			std::printf("PRESET %s: would not load\n", preset);
			failed++;
			engineDeleteInstance(instance);
			continue;
		}
		const int alone = engineVariationCount();
		const std::vector<int32_t> a = play();
		engineDeleteInstance(instance);

		instance = engineNewInstance();
		engineLoadSynth(preset);
		edit();
		const int saveError = engineSaveSynth(edited.c_str());
		engineDeleteInstance(instance);
		// B: the edited file alone.
		instance = engineNewInstance();
		if (saveError != 0 || engineLoadSynth(edited.c_str()) != 0) {
			std::printf("PRESET %s: the edited copy would not save or load (save error %d)\n", preset, saveError);
			failed++;
			engineDeleteInstance(instance);
			continue;
		}
		const std::vector<int32_t> b = play();
		engineDeleteInstance(instance);
		if (apart(a, b) == 0.0) {
			noEffect++; // the edit is not heard in this phrase: the takes below prove little
		}
		// Some sounds do not play the same from one load to the next (the firmware reads a little
		// memory before writing it, CLAUDE.md): the file alone, once more, says whether this is
		// one, and then takes D and F have nothing to be compared with.
		instance = engineNewInstance();
		engineLoadSynth(edited.c_str());
		const bool repeatable = apart(b, play()) == 0.0;
		engineDeleteInstance(instance);
		if (!repeatable) {
			unrepeatable++;
		}

		// C, D, E and F.
		std::vector<int32_t> takes[4];
		bool ok = alone == 1;
		int added = 0, count = 0, currentAfterAdd = -1, resaveError = 0;
		for (int take = 0; take < 3 && ok; take++) {
			instance = engineNewInstance();
			ok = engineLoadSynth(preset) == 0;
			added = ok ? engineVariationAdd(edited.c_str()) : -1;
			count = engineVariationCount();
			currentAfterAdd = engineVariationCurrent();
			ok = ok && added == 1 && count == 2 && currentAfterAdd == 0;
			if (ok && take >= 1) {
				engineVariationUse(1);
				ok = engineVariationCurrent() == 1;
			}
			if (ok && take == 2) {
				engineVariationUse(7); // there is none: nothing happens
				engineVariationUse(0);
				ok = engineVariationCurrent() == 0;
				resaveError = engineVariationSave(1, resaved.c_str());
			}
			if (ok) {
				takes[take] = play();
			}
			engineDeleteInstance(instance);
		}
		if (ok && resaveError == 0) {
			instance = engineNewInstance();
			ok = engineLoadSynth(resaved.c_str()) == 0 && engineVariationCount() == 1;
			if (ok) {
				takes[3] = play();
			}
			engineDeleteInstance(instance);
		}
		if (!ok || resaveError != 0) {
			std::printf("PRESET %s: FAILED (alone %d variations; add gave %d, then %d variations, %d in use; save of variation 1 error %d)\n",
			            preset, alone, added, count, currentAfterAdd, resaveError);
			failed++;
			continue;
		}
		const double c = apart(a, takes[0]), d = repeatable ? apart(b, takes[1]) : 0.0, e = apart(a, takes[2]),
		             f = repeatable ? apart(b, takes[3]) : 0.0;
		const double worst = std::min({c == 0.0 ? 999.0 : c, d == 0.0 ? 999.0 : d, e == 0.0 ? 999.0 : e, f == 0.0 ? 999.0 : f});
		if (worst == 999.0) {
			good++;
		}
		else {
			(worst > 90.0 ? faint : bad)++;
			std::printf("PRESET %s: %s; dB below full scale of the largest difference (0: none): variation 0 against the preset %.1f, "
			            "variation 1 against its file %.1f, 0 again %.1f, variation 1 saved and loaded %.1f\n",
			            preset, worst > 90.0 ? "faintly different" : "DIFFERENT", c, d, e, f);
		}

		// Changing variation while notes sound, every block, with a third variation added on the way.
		instance = engineNewInstance();
		engineLoadSynth(preset);
		engineVariationAdd(edited.c_str());
		int32_t buffer[kMaxRenderFrames * 2];
		int64_t loudest = 0;
		for (int block = 0; block < 240; block++) {
			if (block % 40 == 0) {
				engineNoteOn(48 + block / 40, 100);
			}
			if (block % 40 == 30) {
				engineNoteOff(48 + block / 40);
			}
			if (block == 100) {
				engineVariationAdd(preset);
			}
			engineVariationUse(block % engineVariationCount());
			engineRender(buffer, kMaxRenderFrames);
			for (int32_t sample : buffer) {
				loudest = std::max<int64_t>(loudest, sample < 0 ? -(int64_t)sample : sample);
			}
		}
		if (engineVariationCount() != 3) {
			std::printf("PRESET %s: a third variation was not added while playing\n", preset);
			failed++;
		}
		engineDeleteInstance(instance);
	}
	if (std::getenv("ENGINE_VAR_KEEP") == nullptr) {
		unlink(editedOnDisk.c_str());
		unlink(resavedOnDisk.c_str());
	}
	const int errors = engineFirmwareErrors(firmwareError, sizeof firmwareError) - errorsAtStart;
	std::printf("VARIATIONS %d presets: %d right in all four takes, %d faintly different, %d different, %d failed; "
	            "the edit was not heard in %d; %d edited files do not play the same twice alone (variation 0 only is judged); "
	            "firmware error stops %d%s%s\n",
	            tried, good, faint, bad, failed, noEffect, unrepeatable, errors, errors != 0 ? ", latest " : "", errors != 0 ? firmwareError : "");
	return (bad || failed || errors) ? 1 : 0;
}
