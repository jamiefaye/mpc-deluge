// Deluge Proto - Deluge firmware DSP compiled for the MPC.
//
// This file pulls the firmware's own source files in, unchanged apart from the portability
// patches in ../../patches, and wraps the oscillator and filter in a small interface. The
// firmware is Copyright Synthstrom Audible Limited and contributors, GPL-3.0-or-later; see
// third_party/deluge/LICENSE. That makes this plugin GPL-3.0-or-later too.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "deluge_engine.h"

// --- firmware headers -------------------------------------------------------------------------
#include <definitions_cxx.hpp>
#include <deluge/dsp/filter/filter_set.h>
#include <deluge/dsp/oscillators/oscillator.h>
#include <deluge/processing/engines/audio_engine.h>
#include <deluge/storage/wave_table/wave_table.h>
#include <deluge/util/functions.h>

// --- firmware sources (a unity build: the SDK compiles everything in one command) --------------
#include <deluge/util/lookuptables/analog_square.cpp>
#include <deluge/util/lookuptables/lookuptables.cpp>
#include <deluge/util/lookuptables/mystery_synth_a_saw.cpp>
#include <deluge/util/lookuptables/mystery_synth_b_saw.cpp>
#include <deluge/util/lookuptables/saw.cpp>
#include <deluge/util/lookuptables/square.cpp>
#include <deluge/util/waves.cpp>
#include <deluge/dsp/oscillators/basic_waves.cpp>
#include <deluge/dsp/oscillators/oscillator.cpp>
#include <deluge/dsp/oscillators/sine_osc.cpp>
#include <deluge/dsp/filter/filter.cpp>
#include <deluge/dsp/filter/filter_set.cpp>
#include <deluge/dsp/filter/hpladder.cpp>
#include <deluge/dsp/filter/lpladder.cpp>
#include <deluge/dsp/filter/svf.cpp>

// --- the few firmware symbols that live in files too entangled to compile here ------------------

// From util/functions.cpp (verbatim).
int32_t quickLog(uint32_t input) {
	uint32_t magnitude = getMagnitudeOld(input);
	uint32_t inputLSBs = increaseMagnitude(input, 26 - magnitude);
	return (magnitude << 25) + (inputLSBs & ~((uint32_t)1 << 26));
}

// From util/functions.cpp (verbatim).
int32_t instantTan(int32_t input) {
	int32_t whichValue = input >> 25;                   // 25
	int32_t howMuchFurther = (input << 6) & 2147483647; // 6
	int32_t value1 = tanTable[whichValue];
	int32_t value2 = tanTable[whichValue + 1];
	return (multiply_32x32_rshift32(value2, howMuchFurther)
	        + multiply_32x32_rshift32(value1, 2147483647 - howMuchFurther))
	       << 1;
}

// The firmware lowers quality when its CPU is overloaded. Not wired up here: always "no load".
namespace AudioEngine {
int32_t cpuDireness = 0;
}

// Wavetable oscillators are not part of this prototype; OscType::WAVETABLE is never requested.
uint32_t WaveTable::render(int32_t*, int32_t, uint32_t, uint32_t phase, bool, uint32_t, uint32_t, int32_t, uint32_t,
                           int32_t, int32_t) {
	return phase;
}

// --- the wrapper -------------------------------------------------------------------------------
namespace dproto {

namespace {
constexpr OscType kOscTypes[] = {OscType::SINE,          OscType::TRIANGLE, OscType::SQUARE,
                                 OscType::ANALOG_SQUARE, OscType::SAW,      OscType::ANALOG_SAW_2};
constexpr FilterMode kFilterModes[] = {FilterMode::TRANSISTOR_12DB,       FilterMode::TRANSISTOR_24DB,
                                       FilterMode::TRANSISTOR_24DB_DRIVE, FilterMode::SVF_BAND,
                                       FilterMode::SVF_NOTCH,             FilterMode::OFF};

// volumeNeutralValueForUnison << 1 for one unison voice, as Voice::render passes it.
constexpr int32_t kFilterGainIn = 134217728 << 1;

struct VoiceState {
	uint32_t phase = 0;
	uint32_t phaseIncrement = 0;
	deluge::dsp::filter::FilterSet filterSet;
};
} // namespace

struct Engine::Impl {
	VoiceState voices[kNumVoices];
	// The vector oscillator code may write a few samples past the end; keep slack.
	alignas(16) int32_t buffer[kMaxChunk + 16];
};

Engine::Engine() : impl_(new Impl()) {
	resetAll();
}

Engine::~Engine() {
	delete impl_;
}

void Engine::resetAll() {
	for (VoiceState& v : impl_->voices) {
		v.phase = 0;
		v.phaseIncrement = 0;
		v.filterSet.reset();
	}
}

void Engine::noteOn(int voice, float hz) {
	VoiceState& v = impl_->voices[voice];
	v.phase = 0;
	v.phaseIncrement = (uint32_t)((double)hz * (4294967296.0 / 44100.0));
	v.filterSet.reset();
}

void Engine::render(int voice, float* out, int numFrames, const Params& p, float gainStart, float gainEnd) {
	if (numFrames <= 0 || numFrames > kMaxChunk) {
		return;
	}
	VoiceState& v = impl_->voices[voice];
	int32_t* buffer = impl_->buffer;
	std::memset(buffer, 0, sizeof(int32_t) * (numFrames + 16));

	const OscType oscType = kOscTypes[std::clamp(p.oscType, 0, 5)];
	const FilterMode filterMode = kFilterModes[std::clamp(p.filterMode, 0, 5)];

	// Cutoff: the filters take tan(pi * f / fs) from a table indexed by frequency >> 20, where
	// 2^26 is the Nyquist frequency.
	const float nyquist = 22050.0f;
	int32_t lpfFrequency = (int32_t)(std::clamp(p.cutoffHz, 20.0f, 20000.0f) / nyquist * 67108864.0f);
	// Resonance: the ladder shifts this left by 2 and treats the result as 0..1 in Q31.
	int32_t lpfResonance = (int32_t)(std::clamp(p.resonance, 0.0f, 1.0f) * 0.95f * 536870911.0f);

	int32_t filterGain = v.filterSet.setConfig(lpfFrequency, lpfResonance, filterMode, 0, 0, 0, FilterMode::OFF, 0,
	                                           kFilterGainIn, FilterRoute::HIGH_TO_LOW, false, nullptr);
	// Oscillator level, as Voice::render works it out: default oscillator volume (2^27) times the
	// gain the filter asks for.
	int32_t amplitude = multiply_32x32_rshift32_rounded(134217728, filterGain);

	uint32_t pulseWidth = (uint32_t)(std::clamp(p.pulseWidth, 0.0f, 0.95f) * 2147483647.0f);

	deluge::dsp::Oscillator::renderOsc(oscType, amplitude, buffer, buffer + numFrames, numFrames, v.phaseIncrement,
	                                   pulseWidth, &v.phase, true, 0, false, 0, 0, 0, 0, 0, nullptr);

	v.filterSet.renderLong(std::span{buffer, (size_t)numFrames});

	// Fixed point to float. The scale is chosen by ear and meter, not taken from the firmware:
	// 2^-23 puts one full-velocity saw voice near -8 dBFS before the volume parameter.
	const float scale = 1.0f / 8388608.0f;
	const float gainStep = (gainEnd - gainStart) / (float)numFrames;
	float gain = gainStart;
	for (int i = 0; i < numFrames; ++i) {
		out[i] += (float)buffer[i] * scale * gain;
		gain += gainStep;
	}
}

void Engine::selfTest(std::string& report) {
	static const char* const oscNames[] = {"sine", "triangle", "square", "ana-square", "saw", "ana-saw"};
	static const char* const filterNames[] = {"12dB", "24dB", "drive", "svf-band", "svf-notch", "off"};
	char line[400];
	auto add = [&](const char* text) { report += text; };

	std::snprintf(line, sizeof line, "pointer %d bits, ARMv7a asm paths %s, clang %s\n", (int)(sizeof(void*) * 8),
	              ARMv7a ? "on" : "off", __clang_version__);
	add(line);
	std::snprintf(line, sizeof line,
	              "multiply_32x32_rshift32(2^30,2^30)=%d (want 268435456)  signed_saturate<24>(2^30)=%d (want 8388607)  "
	              "lshiftAndSaturate<5>(2^28)=%d (want 2147483647)  add_saturate(2^30,2^30)=%d (want 2147483647)\n",
	              (int)multiply_32x32_rshift32(1 << 30, 1 << 30), (int)signed_saturate<24>(1 << 30),
	              (int)lshiftAndSaturate<5>(1 << 28), (int)add_saturate(1 << 30, 1 << 30));
	add(line);
	const auto [tableNumber, tableSize] = deluge::dsp::getTableNumber((uint32_t)(261.6256 * (4294967296.0 / 44100.0)));
	const int16_t* sawTable = deluge::dsp::sawTables[tableNumber];
	std::snprintf(line, sizeof line,
	              "quickLog(1<<20)=%d  instantTan(1<<28)=%d  tanTable[8]=%d  C4 saw table number=%d size=%d pointer %s [100]=%d\n",
	              (int)quickLog(1u << 20), (int)instantTan(1 << 28), (int)tanTable[8], (int)tableNumber, (int)tableSize,
	              sawTable ? "ok" : "NULL", sawTable ? (int)sawTable[100] : 0);
	add(line);

	// The ladder filters add noise from the firmware's global random generator (jcong, in
	// util/waves.cpp). Start it from its power-on value so the numbers below are repeatable and
	// comparable between machines, and put it back afterwards.
	const uint32_t savedRandomState = jcong;
	jcong = 380116160;

	VoiceState* v = new VoiceState();
	int32_t* buffer = new int32_t[kMaxChunk + 16];
	const uint32_t phaseIncrement = (uint32_t)(261.6256 * (4294967296.0 / 44100.0));

	for (int osc = 0; osc < 6; ++osc) {
		for (int mode = 0; mode < 6; ++mode) {
			if (osc != 4 && mode != 1) {
				continue; // every oscillator through the 24 dB ladder, and the saw through every filter
			}
			v->phase = 0;
			v->filterSet.reset();
			int64_t oscPeak = 0, outPeak = 0;
			int32_t filterGain = 0, amplitude = 0;
			int32_t firstOsc[4] = {0, 0, 0, 0}, firstOut[4] = {0, 0, 0, 0};
			for (int block = 0; block < 64; ++block) { // 64 * 128 samples = 186 ms
				std::memset(buffer, 0, sizeof(int32_t) * (kMaxChunk + 16));
				filterGain = v->filterSet.setConfig((int32_t)(2500.0f / 22050.0f * 67108864.0f), (int32_t)(0.3f * 0.95f * 536870911.0f),
				                                    kFilterModes[mode], 0, 0, 0, FilterMode::OFF, 0, kFilterGainIn,
				                                    FilterRoute::HIGH_TO_LOW, false, nullptr);
				amplitude = multiply_32x32_rshift32_rounded(134217728, filterGain);
				deluge::dsp::Oscillator::renderOsc(kOscTypes[osc], amplitude, buffer, buffer + kMaxChunk, kMaxChunk,
				                                   phaseIncrement, 0, &v->phase, true, 0, false, 0, 0, 0, 0, 0, nullptr);
				for (int i = 0; i < kMaxChunk; ++i) {
					oscPeak = std::max<int64_t>(oscPeak, std::abs((int64_t)buffer[i]));
				}
				if (block == 0) {
					std::memcpy(firstOsc, buffer + 8, sizeof firstOsc);
				}
				v->filterSet.renderLong(std::span{buffer, (size_t)kMaxChunk});
				for (int i = 0; i < kMaxChunk; ++i) {
					outPeak = std::max<int64_t>(outPeak, std::abs((int64_t)buffer[i]));
				}
				if (block == 0) {
					std::memcpy(firstOut, buffer + 8, sizeof firstOut);
				}
			}
			std::snprintf(line, sizeof line,
			              "%-10s %-9s filterGain=%d amplitude=%d oscPeak=%lld outPeak=%lld endPhase=%u osc[8..11]=%d,%d,%d,%d "
			              "out[8..11]=%d,%d,%d,%d\n",
			              oscNames[osc], filterNames[mode], (int)filterGain, (int)amplitude, (long long)oscPeak,
			              (long long)outPeak, (unsigned)v->phase, (int)firstOsc[0], (int)firstOsc[1], (int)firstOsc[2],
			              (int)firstOsc[3], (int)firstOut[0], (int)firstOut[1], (int)firstOut[2], (int)firstOut[3]);
			add(line);
		}
	}
	delete[] buffer;
	delete v;
	jcong = savedRandomState;
}

} // namespace dproto
