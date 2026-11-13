// Play one SYNTH preset through the engine and keep the audio.
//
// Holds a note for a second, releases it and renders another second, printing a line per stage so
// a crash can be placed. The output goes to /out/engine_smoke.wav (32-bit stereo, exactly what the
// engine produced), which tools/armtest/run.py brings back to build/armtest/out/.
//
//   tools/enginetest.sh [preset.XML]
//
// Arguments: <root folder> <preset path under it> [note] [velocity]
// SPDX-License-Identifier: GPL-3.0-or-later
#include "crash_report.h"
#include "engine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void stage(const char* text) {
	std::fprintf(stderr, "STAGE %s\n", text);
	std::fflush(stderr);
}

int main(int argc, char** argv) {
	crash_report::install();
	const char* root = argc > 1 ? argv[1] : "/sd";
	const char* presetPath = argc > 2 ? argv[2] : "SYNTHS/SYNT100.XML";
	const int note = argc > 3 ? std::atoi(argv[3]) : 60;
	const int velocity = argc > 4 ? std::atoi(argv[4]) : 100;

	stage("start the firmware");
	deluge_hosted::engineInit(root);

	stage("load preset");
	int error = deluge_hosted::engineLoadSynth(presetPath);
	std::fprintf(stderr, "engineLoadSynth(%s) -> %d\n", presetPath, error);
	if (error != 0) {
		return 5;
	}
	int samplesLoaded = 0, samplesFailed = 0;
	deluge_hosted::engineSampleCounts(&samplesLoaded, &samplesFailed);
	std::fprintf(stderr, "samples: %d in memory, %d failed\n", samplesLoaded, samplesFailed);

	stage("play");
	const int frames = deluge_hosted::kMaxRenderFrames;
	const int numBlocks = 689; // two seconds
	static int32_t recording[689 * 128 * 2];
	int64_t peak = 0;
	deluge_hosted::engineNoteOn(note, velocity);
	for (int block = 0; block < numBlocks; block++) {
		// Exercise the expression inputs while the note is held: bend up a tone-ish, wheel, pressure.
		if (block == numBlocks / 8) {
			deluge_hosted::enginePitchBend(4096);
		}
		if (block == numBlocks / 4) {
			deluge_hosted::enginePitchBend(0);
			deluge_hosted::engineModWheel(127);
			deluge_hosted::engineChannelPressure(100);
		}
		if (block == numBlocks / 2) {
			deluge_hosted::engineNoteOff(note);
		}
		int32_t* out = recording + block * frames * 2;
		deluge_hosted::engineRender(out, frames);
		for (int i = 0; i < frames * 2; i++) {
			int64_t a = out[i] < 0 ? -(int64_t)out[i] : out[i];
			if (a > peak) {
				peak = a;
			}
		}
	}
	std::fprintf(stderr, "RESULT peak %.1f dBFS\n", peak ? 20.0 * std::log10((double)peak / 2147483648.0) : -200.0);

	// Where the recording goes: /out in the emulated machine, or wherever ENGINE_SMOKE_OUT says.
	const char* outPath = std::getenv("ENGINE_SMOKE_OUT");
	if (FILE* f = std::fopen(outPath ? outPath : "/out/engine_smoke.wav", "wb")) {
		const uint32_t dataBytes = sizeof recording, rate = 44100, byteRate = rate * 8;
		const uint32_t riffSize = 36 + dataBytes, fmtSize = 16;
		const uint16_t pcm = 1, channels = 2, blockAlign = 8, bits = 32;
		std::fwrite("RIFF", 1, 4, f);
		std::fwrite(&riffSize, 4, 1, f);
		std::fwrite("WAVEfmt ", 1, 8, f);
		std::fwrite(&fmtSize, 4, 1, f);
		std::fwrite(&pcm, 2, 1, f);
		std::fwrite(&channels, 2, 1, f);
		std::fwrite(&rate, 4, 1, f);
		std::fwrite(&byteRate, 4, 1, f);
		std::fwrite(&blockAlign, 2, 1, f);
		std::fwrite(&bits, 2, 1, f);
		std::fwrite("data", 1, 4, f);
		std::fwrite(&dataBytes, 4, 1, f);
		std::fwrite(recording, 1, sizeof recording, f);
		std::fclose(f);
	}
	stage("done");
	return 0;
}
