// Does a sidechain hit from the host duck the sound? For each preset named: hold a note, let it
// settle, give one hit (engineSideChainHit, what the plugin's SC STEPS do), and compare the level
// in the 50 ms after the hit with the 50 ms before it, and again a second later.
//
//     sidechain_hit <card root> <preset under the root> [more presets]
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine.h"
#include "matrix.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

static double level(int blocks) {
	static int32_t out[deluge_hosted::kMaxRenderFrames * 2];
	double sum = 0;
	for (int b = 0; b < blocks; b++) {
		deluge_hosted::engineRender(out, deluge_hosted::kMaxRenderFrames);
		for (int i = 0; i < deluge_hosted::kMaxRenderFrames * 2; i++) {
			sum += (double)out[i] * (double)out[i];
		}
	}
	return 10.0 * std::log10(sum / (blocks * deluge_hosted::kMaxRenderFrames * 2.0) / (2147483648.0 * 2147483648.0) + 1e-30);
}

int main(int argc, char** argv) {
	if (argc < 3) {
		return 2;
	}
	deluge_hosted::engineInit(argv[1]);
	int ducked = 0;
	for (int a = 2; a < argc; a++) {
		if (deluge_hosted::engineLoadSynth(argv[a]) != 0) {
			std::printf("%-44s did not load\n", argv[a]);
			continue;
		}
		// DUCK=<amount> in the environment first sets the "SC Ducking" pad (grid column 10, row 2;
		// -5000..5000), as a user would on the plugin's REVERB page.
		if (const char* duck = std::getenv("DUCK")) {
			deluge_hosted::engineCellSet(0, 10, 2, std::atoi(duck));
		}
		deluge_hosted::engineNoteOn(60, 64);
		level(344);                       // a second, to settle
		const double before = level(17);  // 50 ms
		deluge_hosted::engineSideChainHit();
		const double after = level(17);
		if (std::getenv("PROFILE") != nullptr) { // the level in each 50 ms that follows
			std::printf("   %.1f", after - before);
			for (int i = 0; i < 12; i++) {
				std::printf(" %.1f", level(17) - before);
			}
			std::printf("\n");
		}
		level(344);
		const double later = level(17);
		deluge_hosted::engineNoteOff(60);
		level(172);
		const bool dipped = after < before - 1.0 && later > after + 0.5;
		ducked += dipped;
		std::printf("%-44s before %6.1f dB, after the hit %6.1f, a second on %6.1f%s\n", argv[a], before, after, later,
		            dipped ? "   ducks" : "");
	}
	std::printf("%d of %d duck\n", ducked, argc - 2);
	return 0;
}
