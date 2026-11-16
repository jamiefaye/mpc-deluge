// Several instances in one engine (engine.h, "instances") must not hear one another: a preset
// played in an instance of its own gives exactly the same samples whether it is alone or other
// instances are loading, playing and being deleted between its render blocks.
//
// For every Nth preset in a folder:
//   alone        a new instance, the preset, the test phrase, the instance deleted
//   alone again  the same; differs only if the engine is not repeatable, which would make the
//                rest meaningless (run with DELUGE_SCRUB=165, see CLAUDE.md "Comparing builds")
//   paired       two instances, this preset and the next, rendered block by block in turn
//   disturbed    this preset in one instance while a second loads the next preset part way
//                through, plays it, loads another over it and is deleted before the end
//
//   tools/build_firmware.py native --test tests/engine_instances.cpp
//   DELUGE_SCRUB=165 build/firmware/native/engine_instances build/corpus/root SYNTHS 7
//
// Arguments: <root folder> [folder under it, default SYNTHS] [every Nth preset, default 7]
// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_report.h"
#include "engine.h"

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

// One instance playing the test phrase a block at a time: a chord, every gold knob moved, bend
// and mod wheel, release (the phrase of engine_corpus's "switch" mode).
struct Player {
	int instance = -1;
	int block = 0;
	uint64_t checksum = 1469598103934665603ull;
	std::vector<int32_t> audio; // everything rendered, to say where and by how much two runs differ
	static constexpr int kBlocks = 120;

	bool start(const std::string& preset) {
		instance = engineNewInstance();
		if (instance < 0) {
			std::printf("FAIL  no instance for %s\n", preset.c_str());
			return false;
		}
		return load(preset);
	}
	bool load(const std::string& preset) {
		engineUse(instance);
		block = 0;
		audio.clear();
		checksum = 1469598103934665603ull;
		const int error = engineLoadSynth(preset.c_str());
		if (error != 0) {
			std::printf("FAIL  %s: load error %d\n", preset.c_str(), error);
		}
		return error == 0;
	}
	bool done() const { return block >= kBlocks; }
	void step() {
		if (done()) {
			return;
		}
		engineUse(instance);
		if (block == 0) {
			engineNoteOn(48, 100);
			engineNoteOn(60, 80);
			engineNoteOn(67, 110);
		}
		else if (block == 50) {
			for (int knob = 0; knob < kNumKnobs; knob++) {
				int position = 0;
				if (engineKnobGet(knob, &position)) {
					engineKnobSet(knob, position + (knob % 2 ? -12 : 12));
				}
			}
			enginePitchBend(3000);
			engineModWheel(90);
		}
		else if (block == 90) {
			engineNoteOff(48);
			engineNoteOff(60);
			engineNoteOff(67);
			enginePitchBend(0);
			engineModWheel(0);
		}
		int32_t buffer[kMaxRenderFrames * 2];
		engineRender(buffer, kMaxRenderFrames);
		for (int i = 0; i < kMaxRenderFrames * 2; i++) {
			checksum = (checksum ^ (uint32_t)buffer[i]) * 1099511628211ull;
		}
		audio.insert(audio.end(), buffer, buffer + kMaxRenderFrames * 2);
		block++;
	}
	void finish() {
		engineDeleteInstance(instance);
		instance = -1;
	}
};

// Where two takes first differ and the largest difference, relative to full scale. A difference
// more than 90 dB down is counted apart ("faint"): the firmware reads a few bytes at a sample's
// edge that are whatever lies next to it in memory, and that depends on what else is loaded.
static bool faint = false;
static std::string difference(const std::vector<int32_t>& a, const std::vector<int32_t>& b) {
	size_t first = 0, count = 0;
	int64_t largest = 0;
	for (size_t i = 0; i < a.size() && i < b.size(); i++) {
		const int64_t d = (int64_t)a[i] - (int64_t)b[i];
		if (d != 0) {
			if (count++ == 0) {
				first = i;
			}
			largest = std::max<int64_t>(largest, d < 0 ? -d : d);
		}
	}
	faint = largest != 0 && largest < 68000; // 2^31 / 10^(90/20)
	char text[160];
	std::snprintf(text, sizeof text, " (first at block %d, %d samples differ, largest %.1f dB below full scale)",
	              (int)(first / (kMaxRenderFrames * 2)), (int)count,
	              largest ? -20.0 * std::log10((double)largest / 2147483648.0) : 999.0);
	return text;
}

int main(int argc, char** argv) {
	crash_report::install();
	const char* root = argc > 1 ? argv[1] : "/sd";
	const char* folder = argc > 2 ? argv[2] : "SYNTHS";
	const int every = argc > 3 ? std::max(1, std::atoi(argv[3])) : 7;
	setvbuf(stdout, nullptr, _IONBF, 0);

	std::vector<std::string> all, presets;
	const std::string dirPath = std::string(root) + "/" + folder;
	if (DIR* d = opendir(dirPath.c_str())) {
		while (dirent* e = readdir(d)) {
			const size_t n = std::strlen(e->d_name);
			if (n > 4 && strcasecmp(e->d_name + n - 4, ".xml") == 0) {
				all.push_back(std::string(folder) + "/" + e->d_name);
			}
		}
		closedir(d);
	}
	std::sort(all.begin(), all.end());
	for (size_t i = 0; i < all.size(); i += (size_t)every) {
		presets.push_back(all[i]);
	}
	if (presets.size() < 3) {
		std::printf("FAIL  fewer than three presets in %s\n", dirPath.c_str());
		return 1;
	}

	engineInit(root);
	int notRepeatable = 0, pairedDiffer = 0, disturbedDiffer = 0, failed = 0, faintCount = 0;
	for (size_t i = 0; i < presets.size(); i++) {
		alarm(120);
		const std::string& mine = presets[i];
		const std::string& other = presets[(i + 1) % presets.size()];
		const std::string& third = presets[(i + 2) % presets.size()];
		std::vector<int32_t> lastAlone;
		auto alone = [&](const std::string& preset, uint64_t* sum) {
			Player p;
			if (!p.start(preset)) {
				return false;
			}
			while (!p.done()) {
				p.step();
			}
			*sum = p.checksum;
			lastAlone.swap(p.audio);
			p.finish();
			return true;
		};
		uint64_t first = 0, second = 0, otherAlone = 0;
		std::vector<int32_t> firstAudio;
		if (!alone(mine, &first) || (firstAudio = lastAlone, false) || !alone(mine, &second)
		    || !alone(other, &otherAlone)) {
			failed++;
			continue;
		}
		const bool repeatable = first == second;

		Player a, b;
		if (!a.start(mine) || !b.start(other)) {
			failed++;
			continue;
		}
		while (!a.done() || !b.done()) {
			a.step();
			b.step();
		}
		bool paired = a.checksum == first && b.checksum == otherAlone;
		const std::string pairedHow = a.checksum != first ? difference(firstAudio, a.audio)
		                                                    : " as the second of the pair" + difference(lastAlone, b.audio);
		if (!paired && faint) {
			paired = true;
			faintCount++;
			std::printf("PRESET %s: faintly different when paired%s\n", mine.c_str(), pairedHow.c_str());
		}
		a.finish();
		b.finish();

		bool disturbed = true;
		if (!a.start(mine)) {
			failed++;
			continue;
		}
		while (!a.done()) {
			if (a.block == 20) {
				b.start(other);
			}
			else if (a.block == 70) {
				b.load(third);
			}
			else if (a.block == 110) {
				b.finish();
			}
			a.step();
			if (b.instance >= 0) {
				b.step();
				b.step(); // and not in step with the first
			}
		}
		disturbed = a.checksum == first;
		const std::string disturbedHow = disturbed ? "" : difference(firstAudio, a.audio);
		if (!disturbed && faint) {
			disturbed = true;
			faintCount++;
			std::printf("PRESET %s: faintly different when disturbed%s\n", mine.c_str(), disturbedHow.c_str());
		}
		a.finish();

		notRepeatable += repeatable ? 0 : 1;
		pairedDiffer += (repeatable && !paired) ? 1 : 0;
		disturbedDiffer += (repeatable && !disturbed) ? 1 : 0;
		if (!repeatable || !paired || !disturbed) {
			std::printf("PRESET %s:%s%s%s%s%s\n", mine.c_str(), repeatable ? "" : " not repeatable alone",
			            (repeatable && !paired) ? " differs when paired" : "", (repeatable && !paired) ? pairedHow.c_str() : "",
			            (repeatable && !disturbed) ? " differs when disturbed" : "", disturbedHow.c_str());
		}
	}
	char code[8] = {0};
	const int stops = engineFirmwareErrors(code, sizeof code);
	std::printf("INSTANCES %d presets: %d not repeatable alone, %d differ paired, %d differ disturbed, %d faintly, "
	            "%d would not load; firmware error stops %d %s\n",
	            (int)presets.size(), notRepeatable, pairedDiffer, disturbedDiffer, faintCount, failed, stops, code);
	return (notRepeatable || pairedDiffer || disturbedDiffer || failed || stops) ? 1 : 0;
}
