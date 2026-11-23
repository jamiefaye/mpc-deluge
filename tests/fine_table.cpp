// For ../deluge-force-converter (tools/gen_fine_table.py there): every pad of the shortcut grid
// whose parameter can stand between two steps (matrix.h, engineCellSetFine), with the name its
// parameter has in a Deluge file and the 32-bit value of each of its steps, so a song's
// automation of that parameter can be written as the pad's value. The sound is the one given,
// then the same switched to FM mode for the pads that apply only there.
//
//   tools/build_firmware.py native --test tests/fine_table.cpp
//   build/firmware/native/fine_table <root> <preset under root>
//
// One line a pad, tab-separated:  FINE layer x y min max fileName value value ...
// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_report.h"
#include "engine.h"
#include "matrix.h"

#include <cstdio>

using namespace deluge_hosted;

static bool seen[kGridSlots];

static void pass() {
	for (int layer = 0; layer < kGridLayers; layer++) {
		for (int x = 0; x < kGridColumns; x++) {
			for (int y = 0; y < kGridRows; y++) {
				int32_t min = 0, max = 0, values[129];
				char name[64];
				if (seen[gridSlot(layer, x, y)] || !engineCellFineTable(layer, x, y, &min, &max, values, name, sizeof name)) {
					continue;
				}
				seen[gridSlot(layer, x, y)] = true;
				std::printf("FINE\t%d\t%d\t%d\t%d\t%d\t%s", layer, x, y, (int)min, (int)max, name);
				for (int i = 0; i <= max - min; i++) {
					std::printf("\t%d", (int)values[i]);
				}
				std::printf("\n");
			}
		}
	}
}

int main(int argc, char** argv) {
	crash_report::install();
	if (argc < 3) {
		std::printf("usage: fine_table <root> <preset>\n");
		return 1;
	}
	engineInit(argv[1]);
	if (engineLoadSynth(argv[2]) != 0) {
		std::printf("could not load %s\n", argv[2]);
		return 1;
	}
	pass();
	engineCellSet(0, 6, 4, 1); // Synth Mode: FM
	pass();
	return 0;
}
