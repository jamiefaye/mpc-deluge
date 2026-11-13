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
// With a third argument "switch" it instead plays every preset in one process, one after another
// (see runAllInOneProcess). Every line ends in a checksum of the audio, so two builds can be
// compared with tools/corpus_compare.py; tools/corpus.sh runs all of it on every target.
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
	uint64_t checksum = 1469598103934665603ull; // FNV-1a over every output sample: compares builds bit for bit
	const int blocks = 344; // one second
	deluge_hosted::engineNoteOn(60, 100);
	for (int block = 0; block < blocks; block++) {
		if (block == blocks / 2) {
			deluge_hosted::engineNoteOff(60);
		}
		deluge_hosted::engineRender(buffer, deluge_hosted::kMaxRenderFrames);
		for (int i = 0; i < deluge_hosted::kMaxRenderFrames * 2; i++) {
			int64_t a = buffer[i] < 0 ? -(int64_t)buffer[i] : buffer[i];
			checksum = (checksum ^ (uint32_t)buffer[i]) * 1099511628211ull;
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
	std::printf("PRESET %s: ok peak %.1f dBFS rms %.1f dBFS still sounding %.1f dBFS samples %d+%d failed sum %016llx%s\n",
	            relativePath.c_str(), dB((double)peak), dB(rms), dB((double)peakTail), samplesLoaded, samplesFailed,
	            (unsigned long long)checksum, dB((double)peak) < -100.0 ? " SILENT" : "");
	return 0;
}

// "switch" mode: every preset in turn in ONE process, the way a plugin is used: load over the last
// preset, play a chord, move every gold knob and the bend wheel, release, move on. Presets are
// loaded, played and freed back into the same allocator all the way through, which is what
// exercises the firmware's memory management (and its 32-bit bookkeeping on a 64-bit target).
static int runAllInOneProcess(const char* root, const std::vector<std::string>& presets) {
	crash_report::install();
	deluge_hosted::engineInit(root);
	int32_t buffer[deluge_hosted::kMaxRenderFrames * 2];
	int loaded = 0, failed = 0;
	for (const std::string& preset : presets) {
		alarm(60);
		int error = deluge_hosted::engineLoadSynth(preset.c_str());
		if (error != 0) {
			std::printf("PRESET %s: load error %d\n", preset.c_str(), error);
			failed++;
			continue;
		}
		loaded++;
		uint64_t checksum = 1469598103934665603ull;
		int64_t peak = 0;
		auto render = [&](int blocks) {
			for (int block = 0; block < blocks; block++) {
				deluge_hosted::engineRender(buffer, deluge_hosted::kMaxRenderFrames);
				for (int i = 0; i < deluge_hosted::kMaxRenderFrames * 2; i++) {
					int64_t a = buffer[i] < 0 ? -(int64_t)buffer[i] : buffer[i];
					peak = std::max(peak, a);
					checksum = (checksum ^ (uint32_t)buffer[i]) * 1099511628211ull;
				}
			}
		};
		deluge_hosted::engineNoteOn(48, 100);
		deluge_hosted::engineNoteOn(60, 80);
		deluge_hosted::engineNoteOn(67, 110);
		render(50);
		for (int knob = 0; knob < 16; knob++) {
			int position = 0;
			if (deluge_hosted::engineKnobGet(knob, &position)) {
				deluge_hosted::engineKnobSet(knob, position + (knob % 2 ? -12 : 12));
			}
		}
		deluge_hosted::enginePitchBend(3000);
		deluge_hosted::engineModWheel(90);
		render(40);
		deluge_hosted::engineNoteOff(48);
		deluge_hosted::engineNoteOff(60);
		deluge_hosted::engineNoteOff(67);
		deluge_hosted::enginePitchBend(0);
		deluge_hosted::engineModWheel(0);
		render(30);
		int samplesLoaded = 0, samplesFailed = 0;
		deluge_hosted::engineSampleCounts(&samplesLoaded, &samplesFailed);
		std::printf("PRESET %s: ok peak %.1f dBFS samples %d+%d failed sum %016llx\n", preset.c_str(),
		            peak ? 20.0 * std::log10((double)peak / 2147483648.0) : -200.0, samplesLoaded, samplesFailed,
		            (unsigned long long)checksum);
	}
	std::printf("CORPUS %d presets in one process: %d played, %d load errors\n", (int)presets.size(), loaded, failed);
	return 0;
}

int main(int argc, char** argv) {
	const char* root = argc > 1 ? argv[1] : "/sd";
	const char* folder = argc > 2 ? argv[2] : "SYNTHS";
	const bool oneProcess = argc > 3 && std::strcmp(argv[3], "switch") == 0;
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
	if (oneProcess) {
		return runAllInOneProcess(root, presets);
	}
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
