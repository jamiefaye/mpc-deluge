// See engine.h. The sequence below follows the firmware's own code: deluge_main() for start-up
// and Song::ensureAtLeastOneSessionClip() for "one clip playing one synth preset".
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine.h"
#include "cables.h"

#include "definitions_cxx.hpp"
#include "extern.h"
#include "gui/ui/sound_editor.h"
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
#include "modulation/patch/patch_cable.h"
#include "modulation/patch/patch_cable_set.h"
#include "playback/mode/playback_mode.h"
#include "playback/mode/session.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_instrument.h"
#include "model/sample/sample.h"
#include "modulation/sidechain/sidechain.h"
#include "processing/source.h"
#include "storage/audio/audio_file_holder.h"
#include "storage/multi_range/multi_range.h"
#include "storage/multi_range/multisample_range.h"
#include "util/waves.h"
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
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <strings.h>

// The host's directories (hosted/dirs.c): the system's own header cannot be included here, its
// DIR and the firmware's FatFS DIR being different things with one name.
extern "C" {
int deluge_hosted_dir_open(const char* hostPath);
int deluge_hosted_dir_next(int handle, char* name, size_t nameSize, int* isDir, unsigned long long* size);
void deluge_hosted_dir_close(int handle);
}

// Defined in audio_engine.cpp but, in the firmware's words, "deliberately not in header".
namespace AudioEngine {
void renderAudio(size_t numSamples);
extern std::array<StereoSample, SSI_TX_BUFFER_NUM_SAMPLES> renderingMemory;
extern int32_t masterVolumeAdjustmentL;
extern int32_t masterVolumeAdjustmentR;
// What the firmware's routine() and routine_() keep up to date around each renderAudio() call.
extern volatile int voices_started_this_render;
extern int32_t numSamplesLastTime;
extern int32_t sideChainHitPending;
// The rest of what belongs to one song's sound rather than to the engine: see Instance below.
extern int32_t reverbSidechainVolumeInEffect;
extern int32_t reverbSidechainShapeInEffect;
} // namespace AudioEngine

namespace deluge_hosted {

namespace {
bool started = false;

// The current instance's song, clip and sound. Everything below works on these; engineUse()
// changes which instance they, and the firmware's globals, belong to.
Song* song = nullptr;
InstrumentClip* clip = nullptr;
SoundInstrument* synth = nullptr;
// The loaded sound's clips, one for each variation (engine.h); `clip` is the one in use.
std::vector<InstrumentClip*> variations;
std::vector<Cluster*> residentClusters; // sample data held in memory for the loaded preset
int samplesLoaded = 0, samplesFailed = 0;
// Files the loaded preset names that are not at the path it gives: how many were found
// somewhere else under SAMPLES and loaded from there, and the paths of those not found at all.
int samplesElsewhere = 0;
std::vector<std::string> missingSamples;
std::string rootPath = ".";          // the folder standing in for the card
std::vector<std::string> sampleIndex; // every file under SAMPLES, as a path under the root
bool sampleIndexRead = false;
float tempo = 0; // the host's, in beats a minute, for the instance's song and the songs that replace it; 0: none given

// An instance that is not the current one, put away: its song, and the firmware globals that
// hold a song's sound between renders. The firmware has one of each because a Deluge plays one
// song. Listed by reading AudioEngine::renderAudio and what it calls:
// - the reverb's delay lines, and the sidechain that ducks it, with their settings
// - the sample clock (sounds and voices keep times taken from it) and the times of the last
//   sidechain hit and the last reverb input
// - the output level follower, which keeps the reverb running while there is sound
// - the noise generator (ladder filters and the output dither draw from it), so that what one
//   instance plays does not depend on how much another has rendered
// Not per instance: the voice pools, the sample files and the allocator, which are shared and
// meant to be; the stutter effect's one buffer, which nothing here starts; cpuDireness, 0.
struct Instance {
	bool inUse = false;
	Song* song = nullptr;
	InstrumentClip* clip = nullptr;
	SoundInstrument* synth = nullptr;
	std::vector<InstrumentClip*> variations;
	std::vector<Cluster*> residentClusters;
	int samplesLoaded = 0, samplesFailed = 0;
	int samplesElsewhere = 0;
	std::vector<std::string> missingSamples;
	float tempo = 0;

	deluge::dsp::Reverb* reverb = nullptr;
	SideChain reverbSidechain;
	int32_t reverbSidechainVolume = 0, reverbSidechainShape = 0, reverbPan = 0;
	int32_t reverbSidechainVolumeInEffect = 0, reverbSidechainShapeInEffect = 0;
	bool mustUpdateReverbParamsBeforeNextRender = false;
	int32_t sideChainHitPending = 0;
	uint32_t timeLastSideChainHit = 0;
	int32_t sizeLastSideChainHit = 0;
	uint32_t timeThereWasLastSomeReverb = 0;
	uint32_t audioSampleTimer = 0;
	StereoFloatSample approxRMSLevel{0};
	AbsValueFollower envelopeFollower{};
	uint32_t noise = 0, noiseZ = 0, noiseW = 0;
};
Instance instances[kMaxInstances];
int current = 0;
Instance fresh; // the globals as they stood when the engine had just started: a new instance's

void putAway(Instance& to) {
	to.song = song;
	to.clip = clip;
	to.synth = synth;
	to.variations.swap(variations);
	to.residentClusters.swap(residentClusters);
	to.samplesLoaded = samplesLoaded;
	to.samplesFailed = samplesFailed;
	to.samplesElsewhere = samplesElsewhere;
	to.missingSamples.swap(missingSamples);
	to.tempo = tempo;
	to.reverb = AudioEngine::reverb.current;
	to.reverbSidechain = AudioEngine::reverbSidechain;
	to.reverbSidechainVolume = AudioEngine::reverbSidechainVolume;
	to.reverbSidechainShape = AudioEngine::reverbSidechainShape;
	to.reverbPan = AudioEngine::reverbPan;
	to.reverbSidechainVolumeInEffect = AudioEngine::reverbSidechainVolumeInEffect;
	to.reverbSidechainShapeInEffect = AudioEngine::reverbSidechainShapeInEffect;
	to.mustUpdateReverbParamsBeforeNextRender = AudioEngine::mustUpdateReverbParamsBeforeNextRender;
	to.sideChainHitPending = AudioEngine::sideChainHitPending;
	to.timeLastSideChainHit = AudioEngine::timeLastSideChainHit;
	to.sizeLastSideChainHit = AudioEngine::sizeLastSideChainHit;
	to.timeThereWasLastSomeReverb = AudioEngine::timeThereWasLastSomeReverb;
	to.audioSampleTimer = AudioEngine::audioSampleTimer;
	to.approxRMSLevel = AudioEngine::approxRMSLevel;
	to.envelopeFollower = AudioEngine::envelopeFollower;
	to.noise = jcong;
	to.noiseZ = z;
	to.noiseW = w;
}

void bringBack(Instance& from) {
	song = from.song;
	clip = from.clip;
	synth = from.synth;
	variations.swap(from.variations);
	residentClusters.swap(from.residentClusters);
	samplesLoaded = from.samplesLoaded;
	samplesFailed = from.samplesFailed;
	samplesElsewhere = from.samplesElsewhere;
	missingSamples.swap(from.missingSamples);
	tempo = from.tempo;
	currentSong = song;
	AudioEngine::reverb.current = from.reverb;
	AudioEngine::reverbSidechain = from.reverbSidechain;
	AudioEngine::reverbSidechainVolume = from.reverbSidechainVolume;
	AudioEngine::reverbSidechainShape = from.reverbSidechainShape;
	AudioEngine::reverbPan = from.reverbPan;
	AudioEngine::reverbSidechainVolumeInEffect = from.reverbSidechainVolumeInEffect;
	AudioEngine::reverbSidechainShapeInEffect = from.reverbSidechainShapeInEffect;
	AudioEngine::mustUpdateReverbParamsBeforeNextRender = from.mustUpdateReverbParamsBeforeNextRender;
	AudioEngine::sideChainHitPending = from.sideChainHitPending;
	AudioEngine::timeLastSideChainHit = from.timeLastSideChainHit;
	AudioEngine::sizeLastSideChainHit = from.sizeLastSideChainHit;
	AudioEngine::timeThereWasLastSomeReverb = from.timeThereWasLastSomeReverb;
	AudioEngine::audioSampleTimer = from.audioSampleTimer;
	AudioEngine::approxRMSLevel = from.approxRMSLevel;
	AudioEngine::envelopeFollower = from.envelopeFollower;
	jcong = from.noise;
	z = from.noiseZ;
	w = from.noiseW;
	// The firmware's "what the gold knobs are editing" pointer is into whichever song was last
	// looked at; nothing here reads it, and it must not outlive that song.
	view.activeModControllableModelStack.modControllable = nullptr;
	view.activeModControllableModelStack.setTimelineCounter(nullptr);
	view.activeModControllableModelStack.paramManager = nullptr;
}

// Marks this thread as being inside the engine for the length of a call: see operators.cpp.
struct InEngine {
	InEngine() { deluge_hosted_enter_engine(); }
	~InEngine() { deluge_hosted_leave_engine(); }
};
} // namespace

// Song::setBPM() without the tempo automation it also writes: how long the song's timer tick is.
// Synced delays, LFOs, the arpeggiator and the sidechain's release read the tempo from it.
static void applyTempo() {
	if (tempo <= 0) {
		return;
	}
	double bpm = tempo;
	double samplesPerTick = 110250.0;
	if (song->insideWorldTickMagnitude > 0) {
		bpm *= (double)((uint32_t)1 << song->insideWorldTickMagnitude);
	}
	else if (song->insideWorldTickMagnitude < 0) {
		samplesPerTick *= (double)((uint32_t)1 << (-song->insideWorldTickMagnitude));
	}
	song->setTempoFromNumSamples(samplesPerTick / bpm, false);
}

// setupBlankSong(), without the UI parts.
static void makeBlankSong() {
	void* songMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(Song));
	song = new (songMemory) Song();
	song->paramManager.setupUnpatched();
	GlobalEffectable::initParams(&song->paramManager);
	song->setupDefault();
	currentSong = song;
	applyTempo();
	AudioEngine::getReverbParamsFromSong(currentSong);
	AudioEngine::mustUpdateReverbParamsBeforeNextRender = true;
	clip = nullptr;
	synth = nullptr;
	variations.clear();
}

void engineInit(const char* root) {
	InEngine inEngine;
	deluge_hosted_set_root(root);
	if (rootPath != root) {
		rootPath = root;
		sampleIndexRead = false;
	}
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

	// Instance 0. What the globals hold now is what every later instance starts from.
	putAway(fresh);
	fresh.residentClusters.swap(residentClusters);
	fresh.variations.swap(variations);
	instances[0].inUse = true;
	current = 0;
	makeBlankSong();
}

void engineUse(int instance) {
	InEngine inEngine;
	if (!started || instance == current || instance < 0 || instance >= kMaxInstances || !instances[instance].inUse) {
		return;
	}
	putAway(instances[current]);
	current = instance;
	bringBack(instances[current]);
}

int engineNewInstance() {
	InEngine inEngine;
	if (!started) {
		return -1;
	}
	int id = 1;
	while (id < kMaxInstances && instances[id].inUse) {
		id++;
	}
	if (id == kMaxInstances) {
		return -1;
	}
	// Its reverb is ordinary memory, not the firmware allocator's: it is a megabyte or so that
	// lives as long as the instance, and nothing in the firmware keys on its address.
	void* reverbMemory = nullptr;
	if (posix_memalign(&reverbMemory, alignof(deluge::dsp::Reverb) < 16 ? 16 : alignof(deluge::dsp::Reverb),
	                   sizeof(deluge::dsp::Reverb))
	    != 0) {
		return -1;
	}
	putAway(instances[current]);
	current = id;
	instances[id] = Instance{};
	instances[id].inUse = true;
	Instance start = fresh; // bringBack() swaps the cluster list out of what it is given
	start.reverb = new (reverbMemory) deluge::dsp::Reverb();
	bringBack(start);
	makeBlankSong();
	return id;
}

static void releaseResidentSamples();

void engineDeleteInstance(int instance) {
	InEngine inEngine;
	if (!started || instance < 1 || instance >= kMaxInstances || !instances[instance].inUse) {
		return;
	}
	if (instance != current) {
		putAway(instances[current]);
		current = instance;
		bringBack(instances[current]);
	}
	if (synth != nullptr) {
		synth->killAllVoices();
	}
	releaseResidentSamples();
	Song* toDelete = song;
	deluge::dsp::Reverb* itsReverb = AudioEngine::reverb.current;
	// Instance 0 becomes current before the song is deleted: destructors on the way down ask the
	// current song questions, so it must be a live one that is not being deleted.
	instances[instance] = Instance{};
	current = 0;
	bringBack(instances[0]);
	void* toDealloc = dynamic_cast<void*>(toDelete);
	toDelete->~Song();
	delugeDealloc(toDealloc);
	itsReverb->~Reverb();
	std::free(itsReverb);
}

int engineCurrentInstance() {
	return current;
}

// --- samples that are not where the preset says ---------------------------------------------------
// A preset names its samples by path under the card's root ("SAMPLES/Kicks/909.wav"). People
// copying a card's files to another drive put folders in other places, and a Deluge does not
// tell upper case from lower where a Linux drive does. So when a path leads nowhere, the file is
// looked for under SAMPLES: first at the same path whatever the case of its letters, then by
// its name alone, anywhere; of several with the name, the one whose folders agree with the
// preset's furthest back from the name.

static void indexSamples(const std::string& relative, int depth) {
	// Names first and folders after, so that only one directory is open at a time.
	const int dir = deluge_hosted_dir_open((rootPath + "/" + relative).c_str());
	if (dir < 0) {
		return;
	}
	std::vector<std::string> folders;
	char name[256];
	int isDir = 0;
	unsigned long long size = 0;
	while (deluge_hosted_dir_next(dir, name, sizeof name, &isDir, &size) != 0) {
		if (name[0] == '.') {
			continue;
		}
		if (isDir != 0) {
			folders.push_back(relative + "/" + name);
		}
		else if (sampleIndex.size() < 100000) {
			sampleIndex.push_back(relative + "/" + name);
		}
	}
	deluge_hosted_dir_close(dir);
	if (depth < 12) {
		for (const std::string& folder : folders) {
			indexSamples(folder, depth + 1);
		}
	}
}

// How many folders, counting back from the file's name, the two paths share. The names are
// known to be the same already.
static int foldersShared(const std::string& a, const std::string& b) {
	int shared = 0;
	size_t endA = a.rfind('/'), endB = b.rfind('/');
	while (endA != std::string::npos && endB != std::string::npos && endA > 0 && endB > 0) {
		const size_t startA = a.rfind('/', endA - 1), startB = b.rfind('/', endB - 1);
		const size_t fromA = startA == std::string::npos ? 0 : startA + 1, fromB = startB == std::string::npos ? 0 : startB + 1;
		if (endA - fromA != endB - fromB || strncasecmp(a.c_str() + fromA, b.c_str() + fromB, endA - fromA) != 0) {
			break;
		}
		shared++;
		endA = startA;
		endB = startB;
	}
	return shared;
}

// Where a file named `wanted` (a path under the root, not there) can be had instead, or "".
static std::string findSampleElsewhere(const char* wanted) {
	while (*wanted == '/') {
		wanted++;
	}
	const std::string path = wanted;
	const size_t slash = path.rfind('/');
	const char* name = wanted + (slash == std::string::npos ? 0 : slash + 1);
	const std::string* best = nullptr;
	int bestShared = -1;
	for (const std::string& have : sampleIndex) {
		if (strcasecmp(have.c_str(), wanted) == 0) {
			return have;
		}
		const size_t haveSlash = have.rfind('/');
		if (strcasecmp(have.c_str() + (haveSlash == std::string::npos ? 0 : haveSlash + 1), name) != 0) {
			continue;
		}
		const int shared = foldersShared(have, path);
		if (shared > bestShared) {
			bestShared = shared;
			best = &have;
		}
	}
	return best != nullptr ? *best : std::string();
}

// After a preset's files have been asked for at the paths it names: for each that was not there,
// load it from where it is, if it is anywhere. The preset then names the place it was found (so
// does a copy saved from here); start, end and loop points stay the preset's.
static void loadSamplesFromElsewhere() {
	samplesElsewhere = 0;
	if (synth == nullptr) {
		return;
	}
	bool readThisTime = false;
	for (int32_t s = 0; s < kNumSources; s++) {
		Source& source = synth->sources[s];
		if (source.oscType != OscType::SAMPLE && source.oscType != OscType::WAVETABLE) {
			continue;
		}
		for (int32_t e = 0; e < source.ranges.getNumElements(); e++) {
			AudioFileHolder* holder = source.ranges.getElement(e)->getAudioFileHolder();
			if (holder == nullptr || holder->audioFile != nullptr || holder->filePath.isEmpty()) {
				continue;
			}
			// The folder is read once, and once more per load that still misses a file: someone
			// may have put it there since.
			if (!sampleIndexRead || !readThisTime) {
				sampleIndex.clear();
				indexSamples("SAMPLES", 0);
				std::sort(sampleIndex.begin(), sampleIndex.end());
				sampleIndexRead = readThisTime = true;
			}
			const std::string found = findSampleElsewhere(holder->filePath.get());
			if (found.empty()) {
				continue;
			}
			const std::string named = holder->filePath.get();
			holder->filePath.set(found.c_str());
			holder->loadFile(source.sampleControls.isCurrentlyReversed(), false, true, CLUSTER_ENQUEUE, nullptr, true);
			if (holder->audioFile != nullptr) {
				samplesElsewhere++;
			}
			else {
				holder->filePath.set(named.c_str()); // it would not load from there either
			}
		}
	}
}

// The Deluge streams samples from its card, a cluster at a time, and may discard clusters it is
// not using. Here every cluster of every sample is loaded now and held: asking for a cluster
// with CLUSTER_LOAD_IMMEDIATELY gives it a "reason to be loaded", and keeping that reason stops
// the firmware ever stealing the memory back.
static void makeSamplesResident() {
	samplesLoaded = 0;
	samplesFailed = 0;
	missingSamples.clear();
	if (synth == nullptr) {
		return;
	}
	// The files this sound's oscillators use, each once. (Not every file the engine knows: other
	// instances have theirs, and each keeps its own hold, so that one changing preset does not
	// take a shared file's data away from another.)
	std::vector<Sample*> samples;
	for (int32_t s = 0; s < kNumSources; s++) {
		Source& source = synth->sources[s];
		if (source.oscType != OscType::SAMPLE && source.oscType != OscType::WAVETABLE) {
			continue;
		}
		for (int32_t e = 0; e < source.ranges.getNumElements(); e++) {
			AudioFileHolder* holder = source.ranges.getElement(e)->getAudioFileHolder();
			if (holder == nullptr) {
				continue;
			}
			if (holder->audioFile == nullptr) {
				if (!holder->filePath.isEmpty()) {
					samplesFailed++; // named in the preset, not found under the root
					if (std::find(missingSamples.begin(), missingSamples.end(), holder->filePath.get())
					    == missingSamples.end()) {
						missingSamples.push_back(holder->filePath.get());
					}
				}
				continue;
			}
			if (holder->audioFile->type != AudioFileType::SAMPLE) {
				continue;
			}
			Sample* sample = static_cast<Sample*>(holder->audioFile);
			if (std::find(samples.begin(), samples.end(), sample) == samples.end()) {
				samples.push_back(sample);
			}
		}
	}
	for (Sample* sample : samples) {
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
			// Asked for even if it is already in memory: each call is one more reason to keep it.
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

// The firmware is about to mark this file unused (hosted.h): give up our hold on its clusters, or
// its check that an unused file has no data claimed stops it (E396). This happens when an
// oscillator stops using its sample, for instance on changing from Sample to Wavetable.
static void letGoOf(std::vector<Cluster*>& clusters, void* audioFile) {
	size_t kept = 0;
	for (Cluster* cluster : clusters) {
		if (static_cast<void*>(static_cast<AudioFile*>(cluster->sample)) == audioFile) {
			audioFileManager.removeReasonFromCluster(*cluster, "HOST");
		}
		else {
			clusters[kept++] = cluster;
		}
	}
	clusters.resize(kept);
}

// No sound in any instance uses the file now, so every instance's hold goes.
extern "C" void deluge_hosted_audio_file_unused(void* audioFile) {
	letGoOf(residentClusters, audioFile);
	for (int i = 0; i < kMaxInstances; i++) {
		if (i != current && instances[i].inUse) {
			letGoOf(instances[i].residentClusters, audioFile);
		}
	}
}

int engineFirmwareErrors(char* latest, int size) {
	return deluge_hosted_firmware_errors(latest, size);
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

int engineSamplesFoundElsewhere() {
	return samplesElsewhere;
}

bool engineMissingSample(int index, char* out, int outSize) {
	if (outSize > 0) {
		out[0] = 0;
	}
	if (index < 0 || index >= (int)missingSamples.size()) {
		return false;
	}
	std::snprintf(out, (size_t)outSize, "%s", missingSamples[(size_t)index].c_str());
	return true;
}

void engineSetTempo(float beatsPerMinute) {
	if (!(beatsPerMinute >= 20.0f && beatsPerMinute <= 999.0f) || beatsPerMinute == tempo || song == nullptr) {
		return;
	}
	InEngine inEngine;
	tempo = beatsPerMinute;
	applyTempo();
}

float engineTempo() {
	return song != nullptr ? song->calculateBPM() : 0.0f;
}

extern "C" int deluge_hosted_memory_resident_mb(int* totalMb);
int engineMemoryResidentMb(int* totalMb) {
	return deluge_hosted_memory_resident_mb(totalMb);
}

void engineSideChainHit() {
	// Full strength, which is what a drum loaded from the sample browser sends
	// (SampleBrowser: sideChainSendLevel = 2147483647). engineRender() clears it after one cycle.
	AudioEngine::registerSideChainHit(2147483647);
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
		if (synth != nullptr) {
			synth->killAllVoices(); // this instance's only: AudioEngine::killAllVoices is everybody's
		}
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
	loadSamplesFromElsewhere();
	makeSamplesResident();
	clip->setAudioInstrument(instrument, song, true, &newParamManager);
	song->addOutput(instrument);
	song->setCurrentClip(clip);

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(clip);
	instrument->setActiveClip(modelStack);
	variations.assign(1, clip);
	return 0;
}

// --- variations (engine.h) ------------------------------------------------------------------------------

int engineVariationCount() {
	return synth != nullptr ? (int)variations.size() : 0;
}

int engineVariationCurrent() {
	for (size_t n = 0; n < variations.size(); n++) {
		if (variations[n] == clip) {
			return (int)n;
		}
	}
	return 0;
}

int engineVariationAdd(const char* presetPath) {
	InEngine inEngine;
	if (synth == nullptr || clip == nullptr || variations.empty() || (int)variations.size() >= kMaxVariations) {
		return -1000;
	}
	FilePointer filePointer;
	if (!StorageManager::fileExists(presetPath, &filePointer)) {
		return -(int)Error::FILE_NOT_FOUND;
	}
	// The file is read as engineLoadSynth() reads one, into a new clip and a sound of its own: the
	// firmware's reader then does for these values everything it does for a preset's (formats of
	// older firmware, the volume made up for resonance). The clip keeps the values, the cables
	// and the arpeggiator settings and is given to the loaded sound, as a second clip of one synth
	// is on a Deluge; the sound that came with them is deleted, never having been in the song.
	void* clipMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(InstrumentClip));
	if (clipMemory == nullptr) {
		return -(int)Error::INSUFFICIENT_RAM;
	}
	// A new clip and a sound being read both draw on the noise generator (a clip's colour, for
	// one), which the ladder filters, free-running oscillators and the output dither of the sound
	// that is playing draw on too. It is put back, so that adding a variation changes nothing in
	// what the variation in use plays.
	const uint32_t noise = jcong, noiseZ = z, noiseW = w;
	InstrumentClip* added = new (clipMemory) InstrumentClip(song);
	added->loopLength = clip->loopLength;

	ParamManager newParamManager; // deliberately not set up, as in the firmware
	Instrument* instrument = nullptr;
	String presetName;
	presetName.set("PRESET");
	String dirPath;
	dirPath.set("SYNTHS");
	Error error = StorageManager::loadInstrumentFromFile(song, added, OutputType::SYNTH, false, &instrument, &filePointer,
	                                                     &presetName, &dirPath);
	if (error == Error::NONE && instrument == nullptr) {
		error = Error::BUG;
	}
	if (error == Error::NONE) {
		error = added->setAudioInstrument(instrument, song, false, &newParamManager);
	}
	if (instrument != nullptr) {
		added->output = synth;
		instrument->deleteBackedUpParamManagers(song);
		void* toDealloc = dynamic_cast<void*>(instrument);
		instrument->~Instrument();
		delugeDealloc(toDealloc);
	}
	jcong = noise;
	z = noiseZ;
	w = noiseW;
	if (error != Error::NONE || !added->paramManager.containsAnyMainParamCollections()) {
		added->output = nullptr;
		added->~InstrumentClip();
		delugeDealloc(clipMemory);
		return error != Error::NONE ? -(int)error : -(int)Error::FILE_CORRUPTED;
	}
	song->sessionClips.insertClipAtIndex(added, song->sessionClips.getNumElements());
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	synth->setupPatching(setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(added));
	variations.push_back(added);
	return (int)variations.size() - 1;
}

void engineVariationUse(int n) {
	InEngine inEngine;
	if (synth == nullptr || n < 0 || n >= (int)variations.size() || variations[(size_t)n] == clip) {
		return;
	}
	clip = variations[(size_t)n];
	song->setCurrentClip(clip); // the pads' menu items work on the song's current clip (matrix.cpp)
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	static_cast<Instrument*>(synth)->setActiveClip(setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(clip));
}

int engineSampleSet(int source, const char* path) {
	InEngine inEngine;
	if (synth == nullptr || source < 0 || source >= kNumSources) {
		return (int)Error::BUG;
	}
	// The firmware offers the sample browser only in subtractive mode (FileSelector::isRelevant;
	// ring mod takes wavetables, FM nothing). A sample oscillator in another mode is a state the
	// Deluge cannot be put in, and one its synth-mode pad then refuses to leave.
	if (path != nullptr && path[0] != 0 && synth->getSynthMode() != SynthMode::SUBTRACTIVE) {
		return -1;
	}
	// SampleBrowser::claimCurrentFile() for a synth, loading as a sample, without the display.
	Source& osc = synth->sources[source];
	synth->killAllVoices();
	soundEditor.currentMultiRangeIndex = 0; // setOscType() looks at it
	osc.setOscType(OscType::SAMPLE);
	MultiRange* range = osc.getOrCreateFirstRange();
	if (range == nullptr) {
		return (int)Error::INSUFFICIENT_RAM;
	}
	AudioFileHolder* holder = range->getAudioFileHolder();
	holder->setAudioFile(nullptr); // the firmware lets go of the old file; so do we, through the hook above
	if (path == nullptr || path[0] == 0) {
		holder->filePath.clear(); // no file: the zone is empty, as in a new sound
		releaseResidentSamples();
		makeSamplesResident();
		return 0;
	}
	holder->filePath.set(path);
	const Error error = holder->loadFile(osc.sampleControls.isCurrentlyReversed(), true, true, CLUSTER_ENQUEUE, nullptr, false);
	if (error == Error::NONE && holder->audioFile != nullptr && holder->audioFile->type == AudioFileType::SAMPLE) {
		Sample* sample = static_cast<Sample*>(holder->audioFile);
		if (osc.repeatMode != SampleRepeatMode::STRETCH && osc.repeatMode != SampleRepeatMode::LOOP) {
			const int32_t mSec = sample->getLengthInMSec();
			if (sample->fileLoopEndSamples != 0) {
				osc.repeatMode = (static_cast<MultisampleRange*>(range)->sampleHolder.loopEndPos != 0 && mSec < 2002)
				                     ? SampleRepeatMode::ONCE
				                     : SampleRepeatMode::LOOP;
			}
			else {
				osc.repeatMode = mSec < 2002 ? SampleRepeatMode::ONCE : SampleRepeatMode::CUT;
			}
		}
	}
	// What this instance holds in memory is now out of date either way.
	releaseResidentSamples();
	makeSamplesResident();
	return (int)error;
}

bool engineSampleGet(int source, char* out, int outSize) {
	InEngine inEngine;
	if (outSize > 0) {
		out[0] = 0;
	}
	if (synth == nullptr || source < 0 || source >= kNumSources) {
		return false;
	}
	Source& osc = synth->sources[source];
	if (osc.oscType != OscType::SAMPLE || osc.ranges.getNumElements() == 0) {
		return false;
	}
	AudioFileHolder* holder = osc.ranges.getElement(0)->getAudioFileHolder();
	if (holder == nullptr || holder->audioFile == nullptr) {
		return false;
	}
	std::snprintf(out, (size_t)outSize, "%s", holder->filePath.get());
	return true;
}

uint32_t engineSampleIdOf(int source) {
	InEngine inEngine;
	if (synth == nullptr || source < 0 || source >= kNumSources) {
		return 0;
	}
	Source& osc = synth->sources[source];
	if (osc.oscType != OscType::SAMPLE || osc.ranges.getNumElements() == 0) {
		return 0;
	}
	AudioFileHolder* holder = osc.ranges.getElement(0)->getAudioFileHolder();
	return (holder != nullptr && holder->audioFile != nullptr) ? engineSampleId(holder->filePath.get()) : 0;
}

int engineSaveSynth(const char* presetPath) {
	return engineVariationSave(engineVariationCurrent(), presetPath);
}

int engineVariationSave(int n, const char* presetPath) {
	InEngine inEngine;
	if (synth == nullptr || n < 0 || n >= (int)variations.size()) {
		return (int)Error::BUG;
	}
	InstrumentClip* const clip = variations[(size_t)n];
	// SaveInstrumentPresetUI::performSave(), without the display and the slot bookkeeping.
	Error error = StorageManager::createXMLFile(presetPath, smSerializer, false, false);
	if (error != Error::NONE) {
		return (int)error;
	}
	static_cast<Output*>(synth)->writeToFile(clip, song);
	error = GetSerializer().closeFileAfterWriting(presetPath, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "\n</sound>\n");
	return (int)error;
}

// --- patch cables (cables.h) ---------------------------------------------------------------------------

int engineCableList(Cable* out, int max) {
	InEngine inEngine;
	if (synth == nullptr || clip == nullptr) {
		return 0;
	}
	PatchCableSet* set = clip->paramManager.getPatchCableSet();
	int count = 0;
	for (int c = 0; c < set->numPatchCables; c++) {
		PatchCable& cable = set->patchCables[c];
		// Presets from before V3.2 can hold a cable to the placeholder parameter "range" with no
		// cable to attach it to (CLAUDE.md, the firmware bug list). It does nothing; not listed.
		if (cable.destinationParamDescriptor.getJustTheParam() >= deluge::modulation::params::kNumParams) {
			continue;
		}
		if (count < max) {
			ParamDescriptor destination = cable.destinationParamDescriptor;
			out[count].source = (int16_t)cable.from;
			out[count].destination = (int16_t)destination.getJustTheParam();
			out[count].of = destination.isJustAParam() ? (int16_t)-1 : (int16_t)destination.getBottomLevelSource();
			// PatchCableStrength::readCurrentValue(): -(2^30)..2^30 to -5000..5000, rounded.
			out[count].amount = (int32_t)(((int64_t)cable.param.getCurrentValue() * kCableAmountMax + (1 << 29)) >> 30);
		}
		count++;
	}
	return count;
}

void engineCableSet(int source, int destination, int of, int32_t amount) {
	InEngine inEngine;
	namespace params = deluge::modulation::params;
	if (synth == nullptr || clip == nullptr || source < 0 || source >= kNumPatchSources || destination < 0
	    || destination >= params::kNumParams || of >= kNumPatchSources) {
		return;
	}
	amount = std::clamp<int32_t>(amount, -kCableAmountMax, kCableAmountMax);
	// No check of whether the firmware would offer this cable on a Deluge
	// (Sound::maySourcePatchToParam): it refuses by the sound's present state (a filter that is
	// off, a per-voice source on a parameter the whole sound shares), yet preset files hold such
	// cables, inert, and what a file can hold has to be settable here to restore a project.
	// PatchCableStrength::getModelStack() and writeCurrentValue(): the destination with the source
	// added names the cable; asking for it with creation allowed makes it.
	ParamDescriptor descriptor;
	if (of < 0) {
		descriptor.setToHaveParamOnly(destination);
	}
	else {
		descriptor.setToHaveParamAndSource(destination, (PatchSource)of);
	}
	descriptor.addSource((PatchSource)source);
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithThreeMainThings* modelStack = setupModelStackWithSong(modelStackMemory, song)
	                                                ->addTimelineCounter(clip)
	                                                ->addOtherTwoThingsButNoNoteRow(synth, &clip->paramManager);
	ModelStackWithParamCollection* withCollection = clip->paramManager.getPatchCableSet(modelStack);
	ModelStackWithParamId* withId = withCollection->addParamId(descriptor.data);
	ModelStackWithAutoParam* withParam = withId->paramCollection->getAutoParamFromId(withId, amount != 0);
	if (withParam == nullptr || withParam->autoParam == nullptr) {
		return; // no such cable and none wanted, or no room for another
	}
	const int64_t magicConstant = 922337203685477; // (2^30 / 5000) << 32, the firmware's constant
	withParam->autoParam->setCurrentValueInResponseToUserInput((int32_t)((magicConstant * amount) >> 32), withParam);
}

bool engineCableSourceName(int source, char* out, int outSize) {
	if (outSize > 0) {
		out[0] = 0;
	}
	if (source < 0 || source >= kNumPatchSources) {
		return false;
	}
	std::snprintf(out, (size_t)outSize, "%s", getSourceDisplayNameForOLED((PatchSource)source));
	return true;
}

bool engineCableDestinationName(int destination, char* out, int outSize) {
	namespace params = deluge::modulation::params;
	if (outSize > 0) {
		out[0] = 0;
	}
	if (destination < 0 || destination >= params::kNumParams) {
		return false;
	}
	const char* name = params::getPatchedParamDisplayName(destination);
	std::snprintf(out, (size_t)outSize, "%s", name != nullptr ? name : "");
	return true;
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
//
// A knob may control the depth of a patch cable ("LFO1 -> pitch", the vibrato knob of most
// presets), and a cable only exists while its depth is not zero. allowCreation makes one, as
// turning the knob on a Deluge does (View::modEncoderAction asks with the firmware's default,
// which is to create); without it such a knob has no parameter and the result is null.
static ModelStackWithAutoParam* knobParam(char* modelStackMemory, int knob, bool allowCreation) {
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
	ModelStackWithAutoParam* param = synth->getParamFromModEncoder(1 - (knob % 2), modelStack, allowCreation);
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
	ModelStackWithAutoParam* param = knobParam(modelStackMemory, knob, false);
	if (param == nullptr) {
		// A cable that does not exist is a depth of zero: the knob's lowest position. (Reading
		// must not create it: a cable is part of the sound.)
		if (synth != nullptr && knob >= 0 && knob < kNumKnobs
		    && !synth->modKnobs[knob / 2][1 - (knob % 2)].paramDescriptor.isJustAParam()) {
			*position = kKnobMin;
			return true;
		}
		return false;
	}
	const int32_t value = param->autoParam->getValuePossiblyAtPos(0, param);
	*position = std::clamp<int>(param->paramCollection->paramValueToKnobPos(value, param), kKnobMin, kKnobMax);
	return true;
}

void engineKnobSet(int knob, int position) {
	InEngine inEngine;
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	position = std::clamp(position, kKnobMin, kKnobMax);
	// Lowest position of a cable that does not exist: it stays that way.
	ModelStackWithAutoParam* param = knobParam(modelStackMemory, knob, position != kKnobMin);
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
	// One cycle of the firmware's audio routine. renderAudio() is its master render (every output
	// in the song, then the song's reverb fed by each sound's send, then the song-level filter,
	// bit-crusher and compressor), but it is not the whole cycle: AudioEngine::routine() and
	// routine_() do bookkeeping before and after it, and they cannot be called here because they
	// take the amount to render from the codec's buffer position. So the same steps, in the same
	// order:
	//
	// - The firmware starts at most four voices per cycle (allowedToStartVoice) and counts them
	//   from zero each cycle. Without this reset the fifth note-on after a preset load, and every
	//   one after it, was silently dropped. (Upstream met the same thing in its offline stem
	//   export, which also renders without routine_(): firmware issue #5010.)
	AudioEngine::voices_started_this_render = 0;
	// (Not done: setDireness(), which measures lateness against the hardware timer and has no
	// meaning here, so cpuDireness stays 0; and tickSongFinalizeWindows(), the sequencer clock,
	// which is the missing tempo sync.)
	AudioEngine::numSamplesLastTime = numFrames;
	AudioEngine::renderAudio((size_t)numFrames);
	// - A sidechain hit registered by a note-on is for one cycle only; left set, it would hit
	//   again every cycle.
	AudioEngine::sideChainHitPending = 0;
	// - The firmware's clock, in samples. Reverb and effect tails, LFOs catching up after a sound
	//   has been idle, voice stealing order and more are timed by it.
	AudioEngine::audioSampleTimer += (uint32_t)numFrames;
	AudioEngine::bypassCulling = false;

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
