// See engine.h. The sequence below follows the firmware's own code: deluge_main() for start-up
// and Song::ensureAtLeastOneSessionClip() for "one clip playing one synth preset".
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine.h"

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
#include "playback/mode/playback_mode.h"
#include "playback/mode/session.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_instrument.h"
#include "storage/flash_storage.h"
#include "storage/storage_manager.h"
#include "util/functions.h"

#include <cstring>
#include <new>

namespace deluge_hosted {

namespace {
bool started = false;
Song* song = nullptr;
InstrumentClip* clip = nullptr;
SoundInstrument* synth = nullptr;
int32_t reverbBuffer[kMaxRenderFrames];
} // namespace

void engineInit(const char* root) {
	deluge_hosted_set_root(root);
	if (started) {
		return;
	}
	started = true;

	functionsInit();
	currentPlaybackMode = &session;
	// The firmware talks to its display constantly. The seven-segment one only sends bytes to
	// the PIC, which hosted/hardware.c swallows.
	display = new deluge::hid::display::SevenSegment;
	AudioEngine::init();
	FlashStorage::resetSettings();
	runtimeFeatureSettings.init();

	// setupBlankSong(), without the UI parts.
	void* songMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Song));
	song = new (songMemory) Song();
	song->paramManager.setupUnpatched();
	GlobalEffectable::initParams(&song->paramManager);
	song->setupDefault();
	currentSong = song;
	AudioEngine::getReverbParamsFromSong(currentSong);
}

int engineLoadSynth(const char* presetPath) {
	FilePointer filePointer;
	if (!StorageManager::fileExists(presetPath, &filePointer)) {
		return (int)Error::FILE_NOT_FOUND;
	}

	void* clipMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(InstrumentClip));
	clip = new (clipMemory) InstrumentClip(song);
	song->sessionClips.insertClipAtIndex(clip, 0);
	clip->loopLength = kDefaultClipLength << song->insideWorldTickMagnitude;

	ParamManager newParamManager; // deliberately not set up, as in the firmware
	Instrument* instrument = nullptr;
	String presetName;
	presetName.set("PRESET");
	String dirPath;
	dirPath.set("SYNTHS");
	Error error = StorageManager::loadInstrumentFromFile(song, clip, OutputType::SYNTH, false, &instrument, &filePointer,
	                                                     &presetName, &dirPath);
	if (error != Error::NONE) {
		return (int)error;
	}
	if (instrument == nullptr) {
		return (int)Error::BUG;
	}
	synth = static_cast<SoundInstrument*>(instrument);

	instrument->loadAllAudioFiles(true);
	clip->setAudioInstrument(instrument, song, true, &newParamManager);
	song->addOutput(instrument);
	song->setCurrentClip(clip);

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(clip);
	instrument->setActiveClip(modelStack);
	return 0;
}

static void sendNote(bool on, int note, int velocity) {
	if (synth == nullptr) {
		return;
	}
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithThreeMainThings* modelStack = setupModelStackWithSong(modelStackMemory, song)
	                                                ->addTimelineCounter(clip)
	                                                ->addOtherTwoThingsButNoNoteRow(synth, &clip->paramManager);
	synth->sendNote(modelStack, on, note, zeroMPEValues, 0, velocity, 0, 0, 0);
}

void engineNoteOn(int note, int velocity) {
	sendNote(true, note, velocity);
}

void engineNoteOff(int note) {
	sendNote(false, note, 0);
}

void engineRender(int32_t* out, int numFrames) {
	static_assert(sizeof(deluge::dsp::StereoSample<q31_t>) == 2 * sizeof(int32_t));
	std::memset(out, 0, sizeof(int32_t) * 2 * (size_t)numFrames);
	if (synth == nullptr || numFrames < 1 || numFrames > kMaxRenderFrames) {
		return;
	}
	std::memset(reverbBuffer, 0, sizeof reverbBuffer);
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	auto* samples = reinterpret_cast<deluge::dsp::StereoSample<q31_t>*>(out);
	synth->renderOutput(setupModelStackWithSong(modelStackMemory, song),
	                    deluge::dsp::StereoBuffer<q31_t>{samples, (size_t)numFrames}, reverbBuffer, 0, 0, false, true);
	// The firmware's clocks: its sample counter drives envelope and LFO timing between renders.
	AudioEngine::audioSampleTimer += numFrames;
	deluge_hosted_advance_samples((uint32_t)numFrames);
}

} // namespace deluge_hosted
