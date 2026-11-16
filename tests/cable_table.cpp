// Prints the firmware's patch sources and patchable parameters, for tools/gen_cables.py, and for
// one preset its cables; with "roundtrip" it also checks that every cable of the preset can be
// removed and put back, and that a new one can be made.
//   tools/build_firmware.py native --test tests/cable_table.cpp
//   build/firmware/native/cable_table build/corpus/root SYNTHS/SYNT000.XML [roundtrip]
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cables.h"
#include "engine.h"

#include <cstdio>
#include <cstring>

using namespace deluge_hosted;

int main(int argc, char** argv) {
	engineInit(argc > 1 ? argv[1] : "/sd");
	char name[64];
	for (int i = 0; engineCableSourceName(i, name, sizeof name); i++) {
		std::printf("SOURCE %d %s\n", i, name);
	}
	for (int i = 0; engineCableDestinationName(i, name, sizeof name); i++) {
		std::printf("DEST %d %s\n", i, name);
	}
	if (argc < 3 || engineLoadSynth(argv[2]) != 0) {
		return argc < 3 ? 0 : 1;
	}
	Cable cables[kMaxCables];
	const int count = engineCableList(cables, kMaxCables);
	for (int i = 0; i < count; i++) {
		std::printf("CABLE %d -> %d of %d amount %d\n", cables[i].source, cables[i].destination, cables[i].of, (int)cables[i].amount);
	}
	if (argc > 3 && std::strcmp(argv[3], "roundtrip") == 0) {
		int bad = 0;
		for (int i = 0; i < count; i++) {
			engineCableSet(cables[i].source, cables[i].destination, cables[i].of, 0);
		}
		Cable now[kMaxCables];
		int left = engineCableList(now, kMaxCables);
		if (left != 0) {
			std::printf("  %d cables left after removing all\n", left);
			bad++;
		}
		// Plain cables first: a cable on another's depth needs that other to exist.
		for (int pass = 0; pass < 2; pass++) {
			for (int i = 0; i < count; i++) {
				if ((cables[i].of >= 0) == (pass == 1)) {
					engineCableSet(cables[i].source, cables[i].destination, cables[i].of, cables[i].amount);
				}
			}
		}
		left = engineCableList(now, kMaxCables);
		for (int i = 0; i < count; i++) {
			bool found = false;
			for (int k = 0; k < left; k++) {
				found = found || (now[k].source == cables[i].source && now[k].destination == cables[i].destination
				                  && now[k].of == cables[i].of && now[k].amount == cables[i].amount);
			}
			if (!found) {
				std::printf("  not back: %d -> %d of %d amount %d\n", cables[i].source, cables[i].destination, cables[i].of,
				            (int)cables[i].amount);
				bad++;
			}
		}
		if (left != count) {
			std::printf("  %d cables after putting %d back\n", left, count);
			bad++;
		}
		std::printf("ROUNDTRIP %s: %d cables, %s\n", argv[2], count, bad ? "FAILED" : "ok");
		return bad ? 1 : 0;
	}
	return 0;
}
