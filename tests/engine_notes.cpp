// Play a long run of notes on every SYNTH preset in a folder and check the last ones are as loud
// as the first.
//
// The other tests play a note or a chord for a second. This one is the step sequencer: 400
// notes, one after another, about 46 seconds of audio per preset, each preset in a process of
// its own. It exists because of a fault no short test showed: the engine once stopped starting
// voices after the fourth note-on (see engineRender in hosted/engine.cpp), so a pattern played
// once and went quiet.
//
//   tools/build_firmware.py native --test tests/engine_notes.cpp
//   build/firmware/native/engine_notes <card folder> [SYNTHS]
//   (or under tools/armtest/run.py --exe, with --file for the folder)
//
// A preset FAILS when the loudest of its last 40 notes is more than 30 dB below the loudest of its
// first 8. Presets that are silent from the start are reported, not failed (the corpus test
// lists those).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_report.h"
#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static int runPreset(const char* root, const std::string& preset) {
	crash_report::install();
	alarm(120);
	deluge_hosted::engineInit(root);
	if (deluge_hosted::engineLoadSynth(preset.c_str()) != 0) {
		std::printf("NOTES %s: load error\n", preset.c_str());
		return 10;
	}
	const int notes[] = {48, 55, 60, 63, 67, 72, 60, 58};
	const int numNotes = 400;
	int32_t buffer[deluge_hosted::kMaxRenderFrames * 2];
	int64_t early = 0, late = 0;
	for (int n = 0; n < numNotes; n++) {
		const int note = notes[n % 8];
		deluge_hosted::engineNoteOn(note, 100);
		int64_t peak = 0;
		for (int block = 0; block < 40; block++) { // 116 ms a step, the note held for three quarters of it
			if (block == 30) {
				deluge_hosted::engineNoteOff(note);
			}
			deluge_hosted::engineRender(buffer, deluge_hosted::kMaxRenderFrames);
			for (int i = 0; i < deluge_hosted::kMaxRenderFrames * 2; i++) {
				peak = std::max<int64_t>(peak, buffer[i] < 0 ? -(int64_t)buffer[i] : buffer[i]);
			}
		}
		if (n < 8) {
			early = std::max(early, peak);
		}
		if (n >= numNotes - 40) {
			late = std::max(late, peak);
		}
	}
	auto dB = [](int64_t v) { return v > 0 ? 20.0 * std::log10((double)v / 2147483648.0) : -200.0; };
	if (dB(early) < -100.0) {
		std::printf("NOTES %s: silent from the start\n", preset.c_str());
		return 0;
	}
	const bool faded = dB(late) < dB(early) - 30.0;
	std::printf("NOTES %s: first notes %.1f dBFS, last notes %.1f dBFS%s\n", preset.c_str(), dB(early), dB(late),
	            faded ? " FADED" : "");
	return faded ? 11 : 0;
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
	int ok = 0, faded = 0, other = 0;
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
		else if (WIFEXITED(status) && WEXITSTATUS(status) == 11) {
			faded++;
		}
		else {
			other++;
			if (!(WIFEXITED(status) && WEXITSTATUS(status) == 10)) {
				std::printf("NOTES %s: CRASHED (%s %d)\n", preset.c_str(), WIFSIGNALED(status) ? "signal" : "exit",
				            WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
			}
		}
	}
	std::printf("NOTES %d presets: %d kept sounding, %d faded, %d crashed or would not load\n", (int)presets.size(), ok,
	            faded, other);
	return faded || other ? 1 : 0;
}
