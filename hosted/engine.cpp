// See engine.h. The sequence below follows the firmware's own code: deluge_main() for start-up
// and Song::ensureAtLeastOneSessionClip() for "one clip playing one synth preset".
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine.h"

#include "definitions_cxx.hpp"
#include "extern.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "hid/display/seven_segment.h"
#include "hosted.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/instrument_clip.h"
#include "model/global_effectable/global_effectable.h"
#include "model/model_stack.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param.h"
#include "modulation/params/param_collection.h"
#include "modulation/params/param_descriptor.h"
#include "playback/mode/playback_mode.h"
#include "playback/mode/session.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_instrument.h"
#include "model/sample/sample.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"
#include "storage/flash_storage.h"
#include "storage/storage_manager.h"
#include "util/functions.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <vector>
#include <cstdio>
#include <cstring>
#include <new>

// Defined in audio_engine.cpp but, in the firmware's words, "deliberately not in header".
namespace AudioEngine {
void renderAudio(size_t numSamples);
extern std::array<deluge::dsp::StereoSample<q31_t>, SSI_TX_BUFFER_NUM_SAMPLES> renderingMemory;
extern int32_t masterVolumeAdjustmentL;
extern int32_t masterVolumeAdjustmentR;
} // namespace AudioEngine

namespace deluge_hosted {

namespace {
bool started = false;
Song* song = nullptr;
InstrumentClip* clip = nullptr;
SoundInstrument* synth = nullptr;
std::vector<Cluster*> residentClusters; // sample data held in memory for the loaded preset
int samplesLoaded = 0, samplesFailed = 0;

// Marks this thread as being inside the engine for the length of a call: see operators.cpp.
struct InEngine {
	InEngine() { deluge_hosted_enter_engine(); }
	~InEngine() { deluge_hosted_leave_engine(); }
};
} // namespace

// setupBlankSong(), without the UI parts.
static void makeBlankSong() {
	void* songMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Song));
	song = new (songMemory) Song();
	song->paramManager.setupUnpatched();
	GlobalEffectable::initParams(&song->paramManager);
	song->setupDefault();
	currentSong = song;
	AudioEngine::getReverbParamsFromSong(currentSong);
	AudioEngine::mustUpdateReverbParamsBeforeNextRender = true;
	clip = nullptr;
	synth = nullptr;
}

void engineInit(const char* root) {
	InEngine inEngine;
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
	audioFileManager.init(); // "mounts the card": takes the cluster size from the hosted file layer
	FlashStorage::resetSettings();
	runtimeFeatureSettings.init();

	makeBlankSong();
}

// The Deluge streams samples from its card, a cluster at a time, and may discard clusters it is
// not using. Here every cluster of every sample is loaded now and held: asking for a cluster
// with CLUSTER_LOAD_IMMEDIATELY gives it a "reason to be loaded", and keeping that reason stops
// the firmware ever stealing the memory back.
static void makeSamplesResident() {
	samplesLoaded = 0;
	samplesFailed = 0;
	for (Sample* sample : audioFileManager.sampleFiles | std::views::values) {
		bool ok = true;
		int32_t numClusters = sample->clusters.getNumElements();
		// Only the clusters that hold audio. A WAV file can carry other chunks after its audio
		// (loop points, metadata); the firmware refuses to load a cluster with no audio in it.
		if (sample->audioDataLengthBytes != 0 && sample->audioDataLengthBytes != 0x8FFFFFFFFFFFFFFF) {
			const uint64_t audioEnd = (uint64_t)sample->audioDataStartPosBytes + sample->audioDataLengthBytes;
			numClusters = std::min<int32_t>(numClusters, (int32_t)(((audioEnd - 1) >> Cluster::size_magnitude) + 1));
		}
		for (int32_t i = 0; i < numClusters; i++) {
			SampleCluster* slot = sample->clusters.getElement(i);
			if (slot->cluster != nullptr && slot->cluster->loaded) {
				continue; // already in memory (and already counted, if we loaded it)
			}
			Error error = Error::NONE;
			Cluster* cluster = slot->getCluster(sample, (uint32_t)i, CLUSTER_LOAD_IMMEDIATELY, 0xFFFFFFFF, &error);
			if (cluster == nullptr) {
				char text[200];
				std::snprintf(text, sizeof text, "sample %s: cluster %d of %d would not load, error %d\n",
				              sample->filePath.get(), (int)i, (int)numClusters, (int)error);
				deluge_hosted_log(text);
				ok = false;
				break;
			}
			residentClusters.push_back(cluster);
		}
		(ok ? samplesLoaded : samplesFailed)++;
	}
}

static void releaseResidentSamples() {
	for (Cluster* cluster : residentClusters) {
		audioFileManager.removeReasonFromCluster(*cluster, "HOST");
	}
	residentClusters.clear();
}

void engineSampleCounts(int* loaded, int* failed) {
	InEngine inEngine;
	*loaded = samplesLoaded;
	*failed = samplesFailed;
}

int engineLoadSynth(const char* presetPath) {
	InEngine inEngine;
	FilePointer filePointer;
	if (!StorageManager::fileExists(presetPath, &filePointer)) {
		return (int)Error::FILE_NOT_FOUND;
	}

	// Replacing a loaded preset: silence everything, delete the old song (which deletes its clip
	// and instrument, and lets go of their samples) and start again from a blank one. The order is
	// the firmware's when it swaps songs (LoadSongUI, ClearSong): the new song is made current
	// first and the old one deleted after, because destructors on the way down ask the current
	// song questions. Abandoning the old song instead, as this did at first, runs the allocator
	// dry after a hundred or so preset changes.
	if (song != nullptr && (synth != nullptr || clip != nullptr)) {
		AudioEngine::killAllVoices(true);
		releaseResidentSamples();
		view.activeModControllableModelStack.modControllable = nullptr;
		view.activeModControllableModelStack.setTimelineCounter(nullptr);
		view.activeModControllableModelStack.paramManager = nullptr;
		Song* toDelete = song;
		makeBlankSong();
		void* toDealloc = dynamic_cast<void*>(toDelete);
		toDelete->~Song();
		delugeDealloc(toDealloc);
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
	makeSamplesResident();
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
	InEngine inEngine;
	sendNote(true, note, velocity);
}

void engineNoteOff(int note) {
	InEngine inEngine;
	sendNote(false, note, 0);
}

void engineAllNotesOff() {
	InEngine inEngine;
	if (synth == nullptr) {
		return;
	}
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithThreeMainThings* modelStack = setupModelStackWithSong(modelStackMemory, song)
	                                                ->addTimelineCounter(clip)
	                                                ->addOtherTwoThingsButNoNoteRow(synth, &clip->paramManager);
	synth->allNotesOff(modelStack, synth->getArp());
}

// MelodicInstrument::receivedPitchBend / receivedCC / receivedAftertouch reduce to this call for a
// plain (non-MPE) input channel, with these value scalings.
static void channelExpression(int32_t cc, int32_t value) {
	if (synth == nullptr) {
		return;
	}
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(clip);
	synth->processParamFromInputMIDIChannel(cc, value, modelStack);
}

void enginePitchBend(int value) {
	InEngine inEngine;
	channelExpression(CC_NUMBER_PITCH_BEND, (int32_t)std::clamp(value, -8192, 8191) << 18);
}

void engineModWheel(int value) {
	InEngine inEngine;
	channelExpression(CC_NUMBER_Y_AXIS, (int32_t)std::clamp(value, 0, 127) << 24);
}

void engineChannelPressure(int value) {
	InEngine inEngine;
	channelExpression(CC_NUMBER_AFTERTOUCH, (int32_t)std::clamp(value, 0, 127) << 24);
}

// The firmware finds "the parameter under gold knob N" through the instrument's current knob
// button (modKnobMode); this selects the button, asks, and puts the selection back.
static ModelStackWithAutoParam* knobParam(char* modelStackMemory, int knob) {
	if (synth == nullptr || knob < 0 || knob >= kNumKnobs) {
		return nullptr;
	}
	ModelStackWithThreeMainThings* modelStack = setupModelStackWithSong(modelStackMemory, song)
	                                                ->addTimelineCounter(clip)
	                                                ->addOtherTwoThingsButNoNoteRow(synth, &clip->paramManager);
	uint8_t* mode = synth->getModKnobMode();
	const uint8_t saved = *mode;
	*mode = (uint8_t)(knob / 2);
	// The firmware numbers the lower knob 0 and the upper knob 1.
	ModelStackWithAutoParam* param = synth->getParamFromModEncoder(1 - (knob % 2), modelStack, false);
	*mode = saved;
	return (param != nullptr && param->autoParam != nullptr) ? param : nullptr;
}

bool engineKnobName(int knob, char* out, int outSize) {
	InEngine inEngine;
	if (outSize > 0) {
		out[0] = 0;
	}
	if (synth == nullptr || knob < 0 || knob >= kNumKnobs) {
		return false;
	}
	namespace params = deluge::modulation::params;
	const ParamDescriptor& descriptor = synth->modKnobs[knob / 2][1 - (knob % 2)].paramDescriptor;
	if (descriptor.isJustAParam()) {
		const int32_t p = descriptor.getJustTheParam();
		const char* name = p >= params::UNPATCHED_START
		                       ? params::getParamDisplayName(params::Kind::UNPATCHED_SOUND, p - params::UNPATCHED_START)
		                       : params::getPatchedParamDisplayName(p);
		std::snprintf(out, (size_t)outSize, "%s", name != nullptr ? name : "");
	}
	else {
		// A patch cable's depth: "source -> destination".
		ParamDescriptor copy = descriptor;
		const char* destination = params::getPatchedParamDisplayName(copy.getJustTheParam());
		std::snprintf(out, (size_t)outSize, "%s>%s", sourceToStringShort(copy.getBottomLevelSource()),
		              destination != nullptr ? destination : "?");
	}
	return true;
}

bool engineKnobGet(int knob, int* position) {
	InEngine inEngine;
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* param = knobParam(modelStackMemory, knob);
	if (param == nullptr) {
		return false;
	}
	const int32_t value = param->autoParam->getValuePossiblyAtPos(0, param);
	*position = std::clamp<int>(param->paramCollection->paramValueToKnobPos(value, param), kKnobMin, kKnobMax);
	return true;
}

void engineKnobSet(int knob, int position) {
	InEngine inEngine;
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* param = knobParam(modelStackMemory, knob);
	if (param == nullptr) {
		return;
	}
	// As View::modEncoderAction_existentParam() does it, without the display and undo parts.
	position = std::clamp(position, kKnobMin, kKnobMax);
	const int32_t value = param->paramCollection->knobPosToParamValue(position, param);
	param->autoParam->setValuePossiblyForRegion(value, param, 0, 0);
}

void engineRender(int32_t* out, int numFrames) {
	InEngine inEngine;
	std::memset(out, 0, sizeof(int32_t) * 2 * (size_t)numFrames);
	if (song == nullptr || numFrames < 1 || numFrames > kMaxRenderFrames) {
		return;
	}
	// The firmware's master render: every output in the song, then the song's reverb (fed by each
	// sound's send), then the song-level filter, bit-crusher and compressor. This also advances
	// the firmware's sample clock.
	AudioEngine::renderAudio((size_t)numFrames);

	// The firmware's output stage (the end of AudioEngine::routine_): master volume as a 32x32
	// multiply with dither, then eight doublings with saturation on the way to the codec.
	for (int i = 0; i < numFrames; i++) {
		const auto& sample = AudioEngine::renderingMemory[(size_t)i];
		int64_t l = (int64_t)sample.l * (int64_t)AudioEngine::masterVolumeAdjustmentL + (int64_t)getNoise();
		int64_t r = (int64_t)sample.r * (int64_t)AudioEngine::masterVolumeAdjustmentR + (int64_t)getNoise();
		out[2 * i] = lshiftAndSaturate<8>((int32_t)(l >> 32));     // AUDIO_OUTPUT_GAIN_DOUBLINGS
		out[2 * i + 1] = lshiftAndSaturate<8>((int32_t)(r >> 32));
	}
	deluge_hosted_advance_samples((uint32_t)numFrames);
}

} // namespace deluge_hosted
