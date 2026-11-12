// Sizing probe: reference the firmware entry points a synth-patch player needs, then see what the
// linker says is missing once unreferenced code is dropped.
#include "definitions_cxx.hpp"
#include "model/model_stack.h"
#include "processing/sound/sound_instrument.h"
#include "storage/storage_manager.h"
extern "C" int sizing_root(int argc, void* p) {
	static char mem[MODEL_STACK_MAX_SIZE];
	SoundInstrument* s = new SoundInstrument();
	XMLDeserializer* reader = new XMLDeserializer();
	Error e = s->readFromFile(*reader, nullptr, nullptr, 0);
	ModelStack* ms = setupModelStackWithSong(mem, nullptr);
	ModelStackWithThreeMainThings* m3 = (ModelStackWithThreeMainThings*)ms;
	s->sendNote(m3, true, 60, nullptr, 0, 100, 0, 0, 0);
	deluge::dsp::StereoBuffer<q31_t> buf{(deluge::dsp::StereoSample<q31_t>*)p, 128};
	s->renderOutput(ms, buf, nullptr, 0, 0, false, true);
	return (int)e + argc;
}
