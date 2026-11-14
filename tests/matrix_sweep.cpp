// Write every pad of the shortcut grid on every SYNTH preset in a folder (matrix.h).
//
// Each preset gets its own child process. The child loads the preset, holds a chord, and for
// every pad that applies sets the lowest, highest and middle value and then the original again,
// rendering between each and reading the value back. It reports values that do not read back as
// written, pads whose original value could not be restored, and crashes or hangs.
//
//   tools/build_firmware.py native --test tests/matrix_sweep.cpp
//   build/firmware/native/matrix_sweep build/corpus/root SYNTHS
//
// Arguments: <root folder> <folder of presets under it> [first preset number] [how many]
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
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace deluge_hosted;

static int runPreset(const char* root, const std::string& preset) {
	crash_report::install();
	alarm(120);
	engineInit(root);
	const int error = engineLoadSynth(preset.c_str());
	if (error != 0) {
		std::printf("PRESET %s: load error %d\n", preset.c_str(), error);
		return 10;
	}
	int32_t buffer[kMaxRenderFrames * 2];
	engineNoteOn(48, 100);
	engineNoteOn(60, 80);
	engineRender(buffer, kMaxRenderFrames);
	int written = 0, mismatched = 0, notRestored = 0;
	for (int layer = 0; layer < kGridLayers; layer++) {
		for (int x = 0; x < kGridColumns; x++) {
			for (int y = 0; y < kGridRows; y++) {
				CellInfo before;
				if (!engineCellInfo(layer, x, y, &before) || before.kind == CellKind::None || !before.relevant) {
					continue;
				}
				// Where the crash handler will say we were.
				std::fprintf(stderr, "AT %s pad %d,%d,%d %s\n", preset.c_str(), layer, x, y, before.title);
				const int32_t values[4] = {before.min, before.max, before.min + (before.max - before.min) / 2, before.value};
				for (int i = 0; i < 4; i++) {
					engineCellSet(layer, x, y, values[i]);
					written++;
					engineRender(buffer, kMaxRenderFrames);
					CellInfo after;
					engineCellInfo(layer, x, y, &after);
					if (after.value != values[i] || !after.relevant) {
						if (i == 3) {
							notRestored++;
						}
						else {
							mismatched++;
						}
						std::printf("  %s %s pad %d,%d,%d %s (%s): wrote %d, read %d%s\n", i == 3 ? "NOT RESTORED" : "MISMATCH",
						            preset.c_str(), layer, x, y, before.title, before.type, (int)values[i], (int)after.value,
						            after.relevant ? "" : " and it no longer applies");
					}
				}
			}
		}
	}
	engineNoteOff(48);
	engineNoteOff(60);
	for (int i = 0; i < 20; i++) {
		engineRender(buffer, kMaxRenderFrames);
	}
	std::printf("PRESET %s: ok %d writes, %d read back differently, %d not restored\n", preset.c_str(), written, mismatched,
	            notRestored);
	return 0;
}

int main(int argc, char** argv) {
	const char* root = argc > 1 ? argv[1] : "/sd";
	const char* folder = argc > 2 ? argv[2] : "SYNTHS";
	const int first = argc > 3 ? std::atoi(argv[3]) : 0;
	const int count = argc > 4 ? std::atoi(argv[4]) : 1 << 30;
	setvbuf(stdout, nullptr, _IONBF, 0);

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
	int ok = 0, crashed = 0;
	for (int i = first; i < (int)presets.size() && i - first < count; i++) {
		const pid_t pid = fork();
		if (pid == 0) {
			_exit(runPreset(root, presets[(size_t)i]));
		}
		int status = 0;
		waitpid(pid, &status, 0);
		if (WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 10)) {
			ok++;
		}
		else {
			crashed++;
			std::printf("PRESET %s: CRASHED (%s %d)\n", presets[(size_t)i].c_str(), WIFSIGNALED(status) ? "signal" : "exit",
			            WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
		}
	}
	std::printf("SWEEP %d presets: %d completed, %d crashed\n", ok + crashed, ok, crashed);
	return crashed ? 1 : 0;
}
