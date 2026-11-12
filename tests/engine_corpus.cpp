// Run every SYNTH preset in a folder through the engine and report what each one did.
//
// Each preset gets its own child process (the engine holds one preset per process, and a crash
// in one must not stop the rest). The child starts the firmware, loads the preset, holds middle C
// for half a second, releases it, renders another half second, and prints one line.
//
//   tools/build_firmware.py arm32 --test tests/engine_corpus.cpp
//   tools/armtest/run.py --exe build/firmware/arm32/engine_corpus /sd SYNTHS \
//       --file ~/deluge/SDCard/SYNTHS=/sd/SYNTHS
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_report.h"
#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static int runPreset(const char* root, const std::string& relativePath) {
	crash_report::install();
	alarm(60); // a hang becomes SIGALRM
	deluge_hosted::engineInit(root);
	int error = deluge_hosted::engineLoadSynth(relativePath.c_str());
	if (error != 0) {
		std::printf("PRESET %s: load error %d\n", relativePath.c_str(), error);
		return 10;
	}
	int32_t buffer[deluge_hosted::kMaxRenderFrames * 2];
	int64_t peak = 0;
	double sumSquares = 0;
	int64_t peakTail = 0;
	const int blocks = 344; // one second
	deluge_hosted::engineNoteOn(60, 100);
	for (int block = 0; block < blocks; block++) {
		if (block == blocks / 2) {
			deluge_hosted::engineNoteOff(60);
		}
		deluge_hosted::engineRender(buffer, deluge_hosted::kMaxRenderFrames);
		for (int i = 0; i < deluge_hosted::kMaxRenderFrames * 2; i++) {
			int64_t a = buffer[i] < 0 ? -(int64_t)buffer[i] : buffer[i];
			peak = std::max(peak, a);
			sumSquares += (double)buffer[i] * (double)buffer[i];
			if (block >= blocks - 4) {
				peakTail = std::max(peakTail, a);
			}
		}
	}
	double rms = std::sqrt(sumSquares / (blocks * deluge_hosted::kMaxRenderFrames * 2.0));
	auto dB = [](double v) { return v > 0 ? 20.0 * std::log10(v / 2147483648.0) : -200.0; }; // dBFS at the Deluge's converter
	int samplesLoaded = 0, samplesFailed = 0;
	deluge_hosted::engineSampleCounts(&samplesLoaded, &samplesFailed);
	std::printf("PRESET %s: ok peak %.1f dBFS rms %.1f dBFS still sounding %.1f dBFS samples %d+%d failed%s\n",
	            relativePath.c_str(), dB((double)peak), dB(rms), dB((double)peakTail), samplesLoaded, samplesFailed,
	            dB((double)peak) < -100.0 ? " SILENT" : "");
	return 0;
}

int main(int argc, char** argv) {
	const char* root = argc > 1 ? argv[1] : "/sd";
	const char* folder = argc > 2 ? argv[2] : "SYNTHS";
	setvbuf(stdout, nullptr, _IONBF, 0);

	std::vector<std::string> presets;
	std::string dirPath = std::string(root) + "/" + folder;
	if (DIR* d = opendir(dirPath.c_str())) {
		while (dirent* e = readdir(d)) {
			size_t n = std::strlen(e->d_name);
			if (n > 4 && strcasecmp(e->d_name + n - 4, ".xml") == 0) {
				presets.push_back(std::string(folder) + "/" + e->d_name);
			}
		}
		closedir(d);
	}
	std::sort(presets.begin(), presets.end());
	int ok = 0, loadErrors = 0, crashed = 0;
	for (const std::string& preset : presets) {
		pid_t pid = fork();
		if (pid == 0) {
			_exit(runPreset(root, preset));
		}
		int status = 0;
		waitpid(pid, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			ok++;
		}
		else if (WIFEXITED(status) && WEXITSTATUS(status) == 10) {
			loadErrors++;
		}
		else {
			crashed++;
			std::printf("PRESET %s: CRASHED (%s %d)\n", preset.c_str(), WIFSIGNALED(status) ? "signal" : "exit",
			            WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
		}
	}
	std::printf("CORPUS %d presets: %d rendered, %d load errors, %d crashed\n", (int)presets.size(), ok, loadErrors, crashed);
	return crashed ? 1 : 0;
}
