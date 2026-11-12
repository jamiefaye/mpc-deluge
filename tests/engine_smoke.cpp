// First run of the Deluge firmware's own engine outside a Deluge.
//
// Brings the firmware up the way its deluge_main() does, minus the hardware, loads a SYNTH preset
// through the firmware's XML reader, plays notes and renders. Prints a line before each stage so a
// crash can be placed. Run in the emulated arm32 machine:
//
//   tools/build_firmware.py arm32 --test tests/engine_smoke.cpp
//   tools/armtest/run.py --exe build/firmware/arm32/engine_smoke /sd SYNTHS/SYNT100.XML \
//       --file ~/deluge/SDCard/SYNTHS=/sd/SYNTHS
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "crash_report.h"
#include "definitions_cxx.hpp"
#include "extern.h"
#include "hid/display/display.h"
#include "hid/display/seven_segment.h"
#include "hosted.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/instrument_clip.h"
#include "model/global_effectable/global_effectable.h"
#include "model/model_stack.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "model/voice/voice.h"
#include "playback/mode/playback_mode.h"
#include "playback/mode/session.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_instrument.h"
#include "storage/flash_storage.h"
#include "storage/storage_manager.h"
#include "util/functions.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

static void stage(const char* text) {
	std::fprintf(stderr, "STAGE %s\n", text);
	std::fflush(stderr);
}

int main(int argc, char** argv) {
	crash_report::install();
	const char* root = argc > 1 ? argv[1] : "/sd";
	const char* presetPath = argc > 2 ? argv[2] : "SYNTHS/SYNT100.XML";
	stage("main entered (static constructors done)");
	deluge_hosted_set_root(root);

	stage("functionsInit");
	functionsInit();
	currentPlaybackMode = &session;

	stage("display");
	display = new deluge::hid::display::SevenSegment;

	stage("AudioEngine::init");
	AudioEngine::init();

	stage("settings");
	FlashStorage::resetSettings();
	runtimeFeatureSettings.init();

	stage("blank song");
	void* songMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Song));
	Song* song = new (songMemory) Song();
	song->paramManager.setupUnpatched();
	GlobalEffectable::initParams(&song->paramManager);
	song->setupDefault();
	currentSong = song;
	AudioEngine::getReverbParamsFromSong(currentSong);

	// From here on this follows Song::ensureAtLeastOneSessionClip(), the firmware's own recipe for
	// "a song with one clip playing one synth preset", with the preset named by us instead of
	// found by the browser.
	stage("clip");
	void* clipMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(InstrumentClip));
	InstrumentClip* clip = new (clipMemory) InstrumentClip(song);
	song->sessionClips.insertClipAtIndex(clip, 0);
	clip->loopLength = kDefaultClipLength << song->insideWorldTickMagnitude;

	stage("find preset");
	FilePointer filePointer;
	if (!StorageManager::fileExists(presetPath, &filePointer)) {
		std::fprintf(stderr, "preset %s not found under %s\n", presetPath, root);
		return 3;
	}

	stage("load preset");
	ParamManager newParamManager; // deliberately not set up, as in the firmware
	Instrument* instrument = nullptr;
	String presetName;
	presetName.set("PRESET");
	String dirPath;
	dirPath.set("SYNTHS");
	Error error = StorageManager::loadInstrumentFromFile(song, clip, OutputType::SYNTH, false, &instrument, &filePointer,
	                                                     &presetName, &dirPath);
	std::fprintf(stderr, "loadInstrumentFromFile -> %d, instrument %p\n", (int)error, (void*)instrument);
	if (error != Error::NONE || instrument == nullptr) {
		return 5;
	}
	SoundInstrument* synth = static_cast<SoundInstrument*>(instrument);

	stage("load audio files");
	instrument->loadAllAudioFiles(true);

	stage("attach to clip");
	clip->setAudioInstrument(instrument, song, true, &newParamManager);
	song->addOutput(instrument);
	song->setCurrentClip(clip);

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStack* modelStack = setupModelStackWithSong(modelStackMemory, song);
	ModelStackWithTimelineCounter* modelStackWithClip = modelStack->addTimelineCounter(clip);
	instrument->setActiveClip(modelStackWithClip);

	ModelStackWithThreeMainThings* modelStack3 =
	    modelStackWithClip->addOtherTwoThingsButNoNoteRow(synth, &clip->paramManager);

	stage("note on");
	synth->sendNote(modelStack3, true, 60, zeroMPEValues, 0, 100, 0, 0, 0);

	stage("render");
	static deluge::dsp::StereoSample<q31_t> buffer[128];
	static int32_t reverbBuffer[128];
	int64_t peak = 0;
	double sumSquares = 0;
	const int numBlocks = 689; // two seconds; the note is held for the first one
	static int32_t recording[689 * 128 * 2];
	for (int block = 0; block < numBlocks; block++) {
		if (block == 344) {
			synth->sendNote(modelStack3, false, 60, zeroMPEValues, 0, 0, 0, 0, 0);
		}
		std::memset(buffer, 0, sizeof buffer);
		std::memset(reverbBuffer, 0, sizeof reverbBuffer);
		char renderStackMemory[MODEL_STACK_MAX_SIZE];
		synth->renderOutput(setupModelStackWithSong(renderStackMemory, song),
		                    deluge::dsp::StereoBuffer<q31_t>{buffer, 128}, reverbBuffer, 0, 0, false, true);
		deluge_hosted_advance_samples(128);
		for (auto& s : buffer) {
			int64_t a = s.l < 0 ? -(int64_t)s.l : s.l;
			if (a > peak) {
				peak = a;
			}
			sumSquares += (double)s.l * (double)s.l;
		}
		std::memcpy(recording + block * 256, buffer, sizeof buffer);
		if (block == 20) { // a look inside the voice while the note is sounding
			namespace params = deluge::modulation::params;
			for (const auto& voice : synth->voices()) {
				std::fprintf(stderr, "VOICE volume %d oscA vol %d oscB vol %d pitch %d oscA pitch %d oscB pitch %d lpf freq %d res %d\n",
				             (int)voice->paramFinalValues[params::LOCAL_VOLUME],
				             (int)voice->paramFinalValues[params::LOCAL_OSC_A_VOLUME],
				             (int)voice->paramFinalValues[params::LOCAL_OSC_B_VOLUME],
				             (int)voice->paramFinalValues[params::LOCAL_PITCH_ADJUST],
				             (int)voice->paramFinalValues[params::LOCAL_OSC_A_PITCH_ADJUST],
				             (int)voice->paramFinalValues[params::LOCAL_OSC_B_PITCH_ADJUST],
				             (int)voice->paramFinalValues[params::LOCAL_LPF_FREQ],
				             (int)voice->paramFinalValues[params::LOCAL_LPF_RESONANCE]);
				for (int src = 0; src < 2; src++) {
					std::fprintf(stderr, "VOICE source %d: active %d phase increment %u (C4 would be %u) position %u\n", src,
					             (int)voice->unisonParts[0].sources[src].active,
					             (unsigned)voice->unisonParts[0].sources[src].phaseIncrementStoredValue,
					             (unsigned)(261.6256 * 4294967296.0 / 44100.0),
					             (unsigned)voice->unisonParts[0].sources[src].oscPos);
				}
				std::fprintf(stderr, "VOICE envelope 0 source value %d\n", (int)voice->sourceValues[util::to_underlying(PatchSource::ENVELOPE_0)]);
			}
		}
		if (block == 0 || block == 171 || block == 343 || block == numBlocks - 1) {
			std::fprintf(stderr, "block %d: first samples %d %d %d %d, peak so far %lld\n", block, (int)buffer[0].l,
			             (int)buffer[1].l, (int)buffer[2].l, (int)buffer[3].l, (long long)peak);
		}
	}
	std::fprintf(stderr, "RESULT peak %lld (%.1f dB re full scale)\n", (long long)peak,
	             peak ? 20.0 * __builtin_log10((double)peak / 2147483648.0) : -999.0);
	// What the engine produced, untouched: 32-bit stereo PCM at 44.1 kHz.
	if (FILE* f = std::fopen("/out/engine_smoke.wav", "wb")) {
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
