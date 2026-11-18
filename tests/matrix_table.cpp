// For tools/gen_grid.py: load every SYNTH preset in a folder, one after another, and print what
// the firmware says about every pad of the shortcut grid that applies to it, with the text of
// every value (ranges of up to 400 values) or of a few (wider ranges).
//
//   tools/build_firmware.py native --test tests/matrix_table.cpp
//   build/firmware/native/matrix_table build/corpus/root SYNTHS
//
// One line per pad per preset, tab-separated:
//   CELL layer x y kind min max type name title value=text value=text ...
// followed by the value the preset holds there:
//   VALUE layer x y value
// With MATRIX_FM set in the environment, each preset is then switched to FM mode and the pads
// that apply only there are printed too (FMVALUE ...): gen_grid.py reads an empty sound's values
// this way, as the standard that the GRID page marks departures from.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_report.h"
#include "engine.h"
#include "matrix.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

using namespace deluge_hosted;

int main(int argc, char** argv) {
	crash_report::install();
	const char* root = argc > 1 ? argv[1] : "/sd";
	const char* folder = argc > 2 ? argv[2] : "SYNTHS";
	std::vector<std::string> presets;
	const std::string dirPath = std::string(root) + "/" + folder;
	if (DIR* d = opendir(dirPath.c_str())) {
		while (dirent* e = readdir(d)) {
			const size_t n = std::strlen(e->d_name);
			if (n > 4 && strcasecmp(e->d_name + n - 4, ".xml") == 0) {
				presets.push_back(std::string(folder) + "/" + e->d_name);
			}
		}
		closedir(d);
	}
	std::sort(presets.begin(), presets.end());
	engineInit(root);
	static const char* kinds[] = {"none", "number", "choice", "toggle"};
	for (const std::string& preset : presets) {
		if (engineLoadSynth(preset.c_str()) != 0) {
			continue;
		}
		std::printf("PRESET\t%s\n", preset.c_str());
		for (int layer = 0; layer < kGridLayers; layer++) {
			for (int x = 0; x < kGridColumns; x++) {
				for (int y = 0; y < kGridRows; y++) {
					CellInfo info;
					if (!engineCellInfo(layer, x, y, &info)) {
						continue;
					}
					if (info.kind == CellKind::None || !info.relevant) {
						std::printf("SKIP\t%d\t%d\t%d\t%s\t%s\t%s\t%s\n", layer, x, y, kinds[(int)info.kind], info.type,
						            info.name, info.title);
						continue;
					}
					std::printf("CELL\t%d\t%d\t%d\t%s\t%d\t%d\t%s\t%s\t%s", layer, x, y, kinds[(int)info.kind],
					            (int)info.min, (int)info.max, info.type, info.name, info.title);
					const int64_t span = (int64_t)info.max - info.min;
					const int64_t step = span <= 400 ? 1 : span / 64;
					for (int64_t v = info.min; v <= info.max; v += step) {
						char text[48];
						engineCellTextFor(layer, x, y, (int32_t)v, text, sizeof text);
						std::printf("\t%d=%s", (int)v, text);
					}
					std::printf("\n");
					std::printf("VALUE\t%d\t%d\t%d\t%d\n", layer, x, y, (int)info.value);
				}
			}
		}
		if (std::getenv("MATRIX_FM") != nullptr) {
			engineCellSet(0, 6, 4, 1); // Synth Mode: FM
			for (int layer = 0; layer < kGridLayers; layer++) {
				for (int x = 0; x < kGridColumns; x++) {
					for (int y = 0; y < kGridRows; y++) {
						CellInfo info;
						if (engineCellInfo(layer, x, y, &info) && info.kind != CellKind::None && info.relevant) {
							std::printf("FMVALUE\t%d\t%d\t%d\t%d\n", layer, x, y, (int)info.value);
						}
					}
				}
			}

		}
	}
	return 0;
}
