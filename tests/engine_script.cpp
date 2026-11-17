// Drives the engine from a list of commands on standard input, for tools that build presets and
// measure what the engine does with a setting (tools/make_starter.py). One command per line,
// fields separated by '|':
//
//   load|SYNTHS/INIT.XML           load a preset (path under the root)
//   pad|LPF frequency|25           set a grid pad, named by the firmware's menu title; a Choice
//   pad|Osc1 type|Saw                may be given by option name
//   cable|ENV2|LPF frequency|2500  set a patch cable (source and destination by the firmware's names,
//                                    see "names"; amount -5000..5000, 0 removes)
//   tempo|60                       the host's tempo, beats a minute
//   on|60|127    off|60            notes
//   render|40|/tmp/a.raw           render 40 blocks of 128 frames, appending interleaved stereo
//                                    int32 to the file (no file name: render and discard)
//   save|SYNTHS/New.XML            write the sound as a preset (will not overwrite)
//   samples                        print how the preset's sample files fared
//   pads    cables    names        print what there is
//
// Any command that fails stops the program with status 1 and says why.
//
//   tools/build_firmware.py native --test tests/engine_script.cpp
//   build/firmware/native/engine_script <root folder> < commands
// SPDX-License-Identifier: GPL-3.0-or-later
#include "cables.h"
#include "crash_report.h"
#include "engine.h"
#include "matrix.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <strings.h>
#include <vector>

using namespace deluge_hosted;

static int lineNumber = 0;

[[noreturn]] static void fail(const std::string& why) {
	std::fprintf(stderr, "engine_script: line %d: %s\n", lineNumber, why.c_str());
	std::exit(1);
}

static void setPad(const std::string& title, const std::string& value) {
	for (int layer = 0; layer < kGridLayers; layer++) {
		for (int x = 0; x < kGridColumns; x++) {
			for (int y = 0; y < kGridRows; y++) {
				CellInfo info;
				if (!engineCellInfo(layer, x, y, &info) || info.kind == CellKind::None
				    || strcasecmp(info.title, title.c_str()) != 0) {
					continue;
				}
				if (!info.relevant) {
					fail("pad \"" + title + "\" does nothing in this sound as it stands");
				}
				char* end = nullptr;
				long number = std::strtol(value.c_str(), &end, 10);
				if (end == value.c_str() || *end != 0) { // not a number: an option name
					char option[64];
					number = -1;
					for (int i = 0; engineCellOption(layer, x, y, i, option, sizeof option); i++) {
						if (strcasecmp(option, value.c_str()) == 0) {
							number = i;
							break;
						}
					}
					if (number < 0) {
						fail("pad \"" + title + "\" has no option \"" + value + "\"");
					}
				}
				if (number < info.min || number > info.max) {
					fail("pad \"" + title + "\": " + value + " is outside " + std::to_string(info.min) + ".."
					     + std::to_string(info.max));
				}
				engineCellSet(layer, x, y, (int32_t)number);
				engineCellInfo(layer, x, y, &info);
				if (info.value != number) {
					fail("pad \"" + title + "\" reads " + std::to_string(info.value) + " after being set to " + value);
				}
				return;
			}
		}
	}
	fail("no pad titled \"" + title + "\"");
}

static int named(const std::string& name, bool source) {
	char have[64];
	for (int i = 0; source ? engineCableSourceName(i, have, sizeof have) : engineCableDestinationName(i, have, sizeof have);
	     i++) {
		if (have[0] != 0 && strcasecmp(have, name.c_str()) == 0) {
			return i;
		}
	}
	fail(std::string("no cable ") + (source ? "source" : "destination") + " named \"" + name + "\"");
}

int main(int argc, char** argv) {
	crash_report::install();
	engineInit(argc > 1 ? argv[1] : "/sd");
	std::string line;
	static int32_t buffer[kMaxRenderFrames * 2];
	while (std::getline(std::cin, line)) {
		lineNumber++;
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::vector<std::string> f;
		size_t from = 0;
		for (;;) {
			const size_t bar = line.find('|', from);
			f.push_back(line.substr(from, bar == std::string::npos ? bar : bar - from));
			if (bar == std::string::npos) {
				break;
			}
			from = bar + 1;
		}
		const std::string& c = f[0];
		auto need = [&](size_t n) {
			if (f.size() != n + 1) {
				fail(c + " takes " + std::to_string(n) + " fields");
			}
		};
		if (c == "load") {
			need(1);
			const int error = engineLoadSynth(f[1].c_str());
			if (error != 0) {
				fail("load " + f[1] + " -> firmware error " + std::to_string(error));
			}
		}
		else if (c == "pad") {
			need(2);
			setPad(f[1], f[2]);
		}
		else if (c == "cable") {
			need(3);
			const int source = named(f[1], true), destination = named(f[2], false);
			const int amount = std::atoi(f[3].c_str());
			engineCableSet(source, destination, -1, amount);
			Cable cables[kMaxCables];
			const int count = engineCableList(cables, kMaxCables);
			bool found = amount == 0;
			for (int i = 0; i < count && i < kMaxCables; i++) {
				if (cables[i].source == source && cables[i].destination == destination && cables[i].of == -1) {
					found = cables[i].amount == amount;
				}
			}
			if (!found) {
				fail("cable " + f[1] + " -> " + f[2] + " did not take the amount " + f[3]);
			}
		}
		else if (c == "tempo") {
			need(1);
			engineSetTempo((float)std::atof(f[1].c_str()));
			std::printf("TEMPO|%.2f\n", (double)engineTempo());
		}
		else if (c == "on") {
			need(2);
			engineNoteOn(std::atoi(f[1].c_str()), std::atoi(f[2].c_str()));
		}
		else if (c == "off") {
			need(1);
			engineNoteOff(std::atoi(f[1].c_str()));
		}
		else if (c == "render") {
			if (f.size() != 2 && f.size() != 3) {
				fail("render takes a block count and, optionally, a file");
			}
			FILE* out = f.size() == 3 ? std::fopen(f[2].c_str(), "ab") : nullptr;
			if (f.size() == 3 && out == nullptr) {
				fail("cannot write " + f[2]);
			}
			for (int block = std::atoi(f[1].c_str()); block > 0; block--) {
				engineRender(buffer, kMaxRenderFrames);
				if (out != nullptr) {
					std::fwrite(buffer, sizeof buffer, 1, out);
				}
			}
			if (out != nullptr) {
				std::fclose(out);
			}
		}
		else if (c == "save") {
			need(1);
			const int error = engineSaveSynth(f[1].c_str());
			if (error != 0) {
				fail("save " + f[1] + " -> firmware error " + std::to_string(error));
			}
		}
		else if (c == "samples") {
			int loaded = 0, failed = 0;
			engineSampleCounts(&loaded, &failed);
			std::printf("SAMPLES|loaded %d|not loaded %d|found elsewhere %d\n", loaded, failed, engineSamplesFoundElsewhere());
			char path[512];
			for (int i = 0; engineMissingSample(i, path, sizeof path); i++) {
				std::printf("MISSING|%s\n", path);
			}
		}
		else if (c == "pads") {
			for (int layer = 0; layer < kGridLayers; layer++) {
				for (int x = 0; x < kGridColumns; x++) {
					for (int y = 0; y < kGridRows; y++) {
						CellInfo info;
						if (engineCellInfo(layer, x, y, &info) && info.kind != CellKind::None && info.relevant) {
							std::printf("PAD|%s|%d|%s|%d|%d\n", info.title, (int)info.value, info.text, (int)info.min,
							            (int)info.max);
						}
					}
				}
			}
		}
		else if (c == "cables") {
			Cable cables[kMaxCables];
			const int count = engineCableList(cables, kMaxCables);
			char source[64], destination[64], of[64];
			for (int i = 0; i < count && i < kMaxCables; i++) {
				engineCableSourceName(cables[i].source, source, sizeof source);
				engineCableDestinationName(cables[i].destination, destination, sizeof destination);
				of[0] = 0;
				if (cables[i].of >= 0) {
					engineCableSourceName(cables[i].of, of, sizeof of);
				}
				std::printf("CABLE|%s|%s|%s|%d\n", source, destination, of, (int)cables[i].amount);
			}
		}
		else if (c == "names") {
			char name[64];
			for (int i = 0; engineCableSourceName(i, name, sizeof name); i++) {
				std::printf("SOURCE|%s\n", name);
			}
			for (int i = 0; engineCableDestinationName(i, name, sizeof name); i++) {
				std::printf("DESTINATION|%s\n", name);
			}
		}
		else {
			fail("unknown command \"" + c + "\"");
		}
	}
	return 0;
}
