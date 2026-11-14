// Print what the firmware says about every pad of the shortcut grid for one SYNTH preset: the
// menu item's class, name, whether it applies to this preset, range, value and options.
//
//   tools/build_firmware.py native --test tests/matrix_dump.cpp
//   build/firmware/native/matrix_dump build/corpus/sd SYNTHS/SYNT000.XML
//
// With four more arguments it instead sets one pad and prints the level of a held note before and
// after, to hear that a write reaches the sound:
//   build/firmware/native/matrix_dump build/corpus/root SYNTHS/SYNT000.XML 0 8 7 0     (LPF frequency to 0)
//
// Arguments: <root folder> <preset path under it> [layer x y value]
// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_report.h"
#include "engine.h"
#include "matrix.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace deluge_hosted;

int main(int argc, char** argv) {
	crash_report::install();
	const char* root = argc > 1 ? argv[1] : "/sd";
	const char* presetPath = argc > 2 ? argv[2] : "SYNTHS/SYNT000.XML";
	engineInit(root);
	const int error = engineLoadSynth(presetPath);
	if (error != 0) {
		std::fprintf(stderr, "engineLoadSynth(%s) -> %d\n", presetPath, error);
		return 5;
	}
	if (argc > 6) {
		const int layer = std::atoi(argv[3]), x = std::atoi(argv[4]), y = std::atoi(argv[5]), value = std::atoi(argv[6]);
		static int32_t buffer[kMaxRenderFrames * 2];
		auto level = [&]() {
			double sum = 0;
			for (int block = 0; block < 100; block++) {
				engineRender(buffer, kMaxRenderFrames);
				for (int i = 0; i < kMaxRenderFrames * 2; i++) {
					sum += (double)buffer[i] * (double)buffer[i];
				}
			}
			return 10.0 * std::log10(sum / (100.0 * kMaxRenderFrames * 2) / (2147483648.0 * 2147483648.0) + 1e-30);
		};
		CellInfo info;
		engineCellInfo(layer, x, y, &info);
		engineNoteOn(60, 64);
		std::printf("%s = %d \"%s\": %.1f dBFS rms\n", info.title, (int)info.value, info.text, level());
		engineCellSet(layer, x, y, value);
		engineCellInfo(layer, x, y, &info);
		std::printf("%s = %d \"%s\": %.1f dBFS rms\n", info.title, (int)info.value, info.text, level());
		return 0;
	}
	static const char* kinds[] = {"none", "number", "choice", "toggle"};
	for (int layer = 0; layer < kGridLayers; layer++) {
		for (int x = 0; x < kGridColumns; x++) {
			for (int y = 0; y < kGridRows; y++) {
				CellInfo info;
				if (!engineCellInfo(layer, x, y, &info)) {
					continue;
				}
				std::printf("%d %2d %d  %-6s %s %-28s %-22s | %-22s [%d..%d] = %d \"%s\"", layer, x, y,
				            kinds[(int)info.kind], info.relevant ? "Y" : "-", info.type, info.name, info.title,
				            (int)info.min, (int)info.max, (int)info.value, info.text);
				if (info.kind == CellKind::Choice && info.relevant) {
					std::printf("  {");
					char option[48];
					for (int i = 0; engineCellOption(layer, x, y, i, option, sizeof option) && i < 40; i++) {
						std::printf("%s%s", i ? "," : "", option);
					}
					std::printf("}");
				}
				std::printf("\n");
				std::fflush(stdout);
			}
		}
	}
	return 0;
}
