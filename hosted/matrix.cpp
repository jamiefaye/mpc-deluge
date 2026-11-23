// See matrix.h. A pad press on a Deluge ends in SoundEditor::setup(clip, item, x & 1), which
// points the sound editor at the clip's sound and its parameters, and then the menu item reads and
// writes through the sound editor. This does the same pointing, without opening any UI, and then
// uses the item's own readCurrentValue() / writeCurrentValue().
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "matrix.h"

#include "definitions_cxx.hpp"
#include "gui/l10n/language.h"
#include "gui/menu_item/enumeration.h"
#include "gui/menu_item/menu_item.h"
#include "gui/menu_item/number.h"
#include "gui/menu_item/param.h"
#include "gui/menu_item/selection.h"
#include "gui/menu_item/toggle.h"
#include "gui/menu_item/value.h"
#include "gui/ui/menus.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "gui/views/instrument_clip_view.h"
#include "hosted.h"
#include "model/clip/instrument_clip.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "processing/sound/sound_instrument.h"
#include "util/d_stringbuf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cxxabi.h>
#include <typeinfo>

extern int32_t numUIsOpen; // gui/ui/ui.cpp

namespace deluge_hosted {

namespace {
using namespace deluge::gui::menu_item;

struct InEngine {
	InEngine() { deluge_hosted_enter_engine(); }
	~InEngine() { deluge_hosted_leave_engine(); }
};

// The range of a Number and the write of a Value are protected in the firmware. A using-declaration
// in a derived class makes a public name for the same member function, and a pointer to it can be
// called on any object of the base class.
struct NumberAccess : Number {
	using Number::getMaxValue;
	using Number::getMinValue;
};
template <typename T>
struct ValueAccess : Value<T> {
	using Value<T>::writeCurrentValue;
};
int32_t minOf(Number* n) { return (n->*(&NumberAccess::getMinValue))(); }
int32_t maxOf(Number* n) { return (n->*(&NumberAccess::getMaxValue))(); }
template <typename T>
void write(Value<T>* v) { (v->*(&ValueAccess<T>::writeCurrentValue))(); }

MenuItem* itemAt(int layer, int x, int y) {
	if (layer < 0 || layer >= kGridLayers || x < 0 || x >= kGridColumns || y < 0 || y >= kGridRows) {
		return nullptr;
	}
	MenuItem* item = layer == 0 ? paramShortcutsForSounds[x][y] : paramShortcutsForSoundsSecondLayer[x][y];
	// The firmware marks pads "coming soon" with an all-ones pointer.
	if (item == nullptr || (uintptr_t)item == (uintptr_t)0xFFFFFFFF || (uintptr_t)item == ~(uintptr_t)0) {
		return nullptr;
	}
	return item;
}

// While one of these exists the firmware sees what it would see with a synth clip open in clip
// view and the sound editor on `item`: menu items build their model stack from the root UI
// (SoundEditor::getCurrentModelStack). No UI is opened or drawn, and the UI stack is put back
// after, so nothing else in the engine sees a change.
struct Editing {
	bool ok = false;
	int32_t savedNumUIs;
	RootUI* savedRoot;
	deluge::l10n::Language const* savedLanguage;
	Sound* sound = nullptr;
	MultiRange* range = nullptr;

	Editing(MenuItem* item, int x) {
		savedNumUIs = numUIsOpen;
		savedRoot = savedNumUIs > 0 ? getRootUI() : nullptr;
		InstrumentClip* clip = currentSong != nullptr ? getCurrentInstrumentClip() : nullptr;
		if (item == nullptr || clip == nullptr || clip->output == nullptr || clip->output->type != OutputType::SYNTH) {
			return;
		}
		sound = static_cast<SoundInstrument*>(clip->output);
		swapOutRootUILowLevel(&instrumentClipView);
		numUIsOpen = 1;
		// The engine runs with the seven-segment display (engine.cpp), whose strings are four-letter
		// abbreviations. Names and value text are wanted in full, as the OLED model shows them.
		savedLanguage = deluge::l10n::chosenLanguage;
		deluge::l10n::chosenLanguage = &deluge::l10n::built_in::english;

		const int32_t thing = x & 1; // as potentialShortcutPadAction() chooses the source
		soundEditor.currentSound = sound;
		soundEditor.currentModControllable = sound;
		soundEditor.currentParamManager = &clip->paramManager;
		soundEditor.currentArpSettings = &clip->arpSettings;
		soundEditor.currentSidechain = &sound->sidechain;
		soundEditor.currentSourceIndex = (int8_t)thing;
		soundEditor.currentSource = &sound->sources[thing];
		soundEditor.currentSampleControls = &soundEditor.currentSource->sampleControls;
		soundEditor.currentPriority = &sound->voicePriority;
		// A multisample has a range per key zone and the Deluge asks which. Here: the first.
		range = soundEditor.currentSource->ranges.getNumElements() > 0
		            ? soundEditor.currentSource->ranges.getElement(0)
		            : nullptr;
		soundEditor.currentMultiRange = range;
		soundEditor.navigationDepth = 0;
		soundEditor.menuItemNavigationRecord[0] = item;
		ok = true;
	}
	~Editing() {
		if (ok) {
			if (savedRoot != nullptr) {
				swapOutRootUILowLevel(savedRoot);
			}
			numUIsOpen = savedNumUIs;
			deluge::l10n::chosenLanguage = savedLanguage;
		}
	}

	// Whether the pad does anything in this preset: the item's own checkPermissionToBeginSession(),
	// as SoundEditor::setup() asks it. One case is answered here instead: for an item that belongs
	// to a key zone, in a sample or wavetable source that has no zone yet, the firmware would
	// create an empty first zone just by being asked.
	bool relevant(MenuItem* item, int x) {
		const int32_t thing = x & 1;
		Source& source = sound->sources[thing];
		if (range == nullptr) {
			if (!item->isRelevant(sound, thing)) {
				return false;
			}
			if (sound->getSynthMode() != SynthMode::FM
			    && (source.oscType == OscType::SAMPLE || source.oscType == OscType::WAVETABLE)) {
				return false;
			}
		}
		const int32_t zonesBefore = source.ranges.getNumElements();
		MultiRange* asked = range;
		const MenuPermission permission = item->checkPermissionToBeginSession(sound, thing, &asked);
		if (source.ranges.getNumElements() != zonesBefore) {
			deluge_hosted_log("matrix: asking a menu item for permission created a key zone\n");
		}
		return permission != MenuPermission::NO;
	}
};

void copy(char* out, size_t size, std::string_view text) {
	const size_t n = std::min(size - 1, text.size());
	std::memcpy(out, text.data(), n);
	out[n] = 0;
}
} // namespace

bool engineCellInfo(int layer, int x, int y, CellInfo* out) {
	InEngine inEngine;
	std::memset(out, 0, sizeof *out);
	MenuItem* item = itemAt(layer, x, y);
	Editing editing(item, x);
	if (!editing.ok) {
		return false;
	}
	copy(out->name, sizeof out->name, item->getName());
	copy(out->title, sizeof out->title, item->getTitle());
	int status = 0;
	if (char* demangled = abi::__cxa_demangle(typeid(*item).name(), nullptr, nullptr, &status)) {
		const char* tail = std::strstr(demangled, "menu_item::");
		copy(out->type, sizeof out->type, tail != nullptr ? tail + 11 : demangled);
		std::free(demangled);
	}
	const bool isValue = dynamic_cast<Value<int32_t>*>(item) != nullptr || dynamic_cast<Value<bool>*>(item) != nullptr;
	out->relevant = isValue && editing.relevant(item, x);

	if (auto* number = dynamic_cast<Number*>(item)) {
		out->kind = CellKind::Number;
		if (out->relevant) {
			out->min = minOf(number);
			out->max = maxOf(number);
			number->readCurrentValue();
			out->value = number->getValue();
		}
	}
	else if (auto* choice = dynamic_cast<Enumeration*>(item)) {
		out->kind = CellKind::Choice;
		if (out->relevant) {
			out->max = (int32_t)choice->size() - 1;
			choice->readCurrentValue();
			out->value = choice->getValue();
		}
	}
	else if (auto* toggle = dynamic_cast<deluge::gui::menu_item::Toggle*>(item)) {
		out->kind = CellKind::Toggle;
		if (out->relevant) {
			out->max = 1;
			toggle->readCurrentValue();
			out->value = toggle->getValue() ? 1 : 0;
		}
	}
	else {
		out->kind = CellKind::None;
		return true;
	}
	if (out->relevant) {
		if (out->kind == CellKind::Toggle) {
			copy(out->text, sizeof out->text, out->value ? "On" : "Off");
		}
		else {
			DEF_STACK_STRING_BUF(text, 40);
			item->getNotificationValue(text);
			copy(out->text, sizeof out->text, text.c_str());
		}
	}
	return true;
}

void engineCellSet(int layer, int x, int y, int32_t value) {
	InEngine inEngine;
	MenuItem* item = itemAt(layer, x, y);
	Editing editing(item, x);
	const bool isValue = editing.ok
	                     && (dynamic_cast<Value<int32_t>*>(item) != nullptr || dynamic_cast<Value<bool>*>(item) != nullptr);
	if (!isValue || !editing.relevant(item, x)) {
		return;
	}
	if (auto* number = dynamic_cast<Number*>(item)) {
		number->readCurrentValue();
		number->setValue(std::clamp(value, minOf(number), maxOf(number)));
		write<int32_t>(number);
	}
	else if (auto* choice = dynamic_cast<Enumeration*>(item)) {
		choice->readCurrentValue();
		choice->setValue(std::clamp<int32_t>(value, 0, (int32_t)choice->size() - 1));
		write<int32_t>(choice);
	}
	else if (auto* toggle = dynamic_cast<deluge::gui::menu_item::Toggle*>(item)) {
		toggle->readCurrentValue();
		toggle->setValue(value != 0);
		write<bool>(toggle);
	}
}

namespace {
// The 32-bit value the firmware gives each step of a pad, found once by writing every step
// through the pad's own menu item and reading the parameter (so the scaling is the firmware's,
// whichever kind of item it is), then putting the parameter back. Fixed for a pad: it does not
// depend on the sound. Only for ranges of up to kFineSteps steps.
constexpr int kFineSteps = 129;
struct FineTable {
	uint8_t state = 0; // 0 not looked at, 1 usable, 2 not this kind of pad
	int32_t min = 0, max = 0;
	int32_t at[kFineSteps];
};
FineTable fineTables[kGridSlots];

// The parameter behind a pad, or null. `memory` is the model stack's.
ModelStackWithAutoParam* paramOf(MenuItem* item, void* memory) {
	auto* param = dynamic_cast<Param*>(item);
	if (param == nullptr) {
		return nullptr;
	}
	ModelStackWithAutoParam* modelStack = param->getModelStack(memory);
	return modelStack != nullptr && modelStack->autoParam != nullptr ? modelStack : nullptr;
}

// With an Editing in place for `item`, relevant. Null if the pad is not of this kind.
FineTable* fineTableOf(int layer, int x, int y, MenuItem* item) {
	FineTable& table = fineTables[gridSlot(layer, x, y)];
	if (table.state != 0) {
		return table.state == 1 ? &table : nullptr;
	}
	table.state = 2;
	auto* number = dynamic_cast<Number*>(item);
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* modelStack = number != nullptr ? paramOf(item, memory) : nullptr;
	if (modelStack == nullptr) {
		return nullptr;
	}
	table.min = minOf(number);
	table.max = maxOf(number);
	if (table.max <= table.min || table.max - table.min >= kFineSteps) {
		return nullptr;
	}
	const int32_t before = modelStack->autoParam->getCurrentValue();
	for (int32_t step = table.min; step <= table.max; step++) {
		number->setValue(step);
		write<int32_t>(number);
		table.at[step - table.min] = modelStack->autoParam->getCurrentValue();
	}
	modelStack->autoParam->setCurrentValueInResponseToUserInput(before, modelStack);
	// It has to run one way and get somewhere, or a place between two steps means nothing.
	const int n = table.max - table.min;
	const bool rising = table.at[n] > table.at[0];
	for (int i = 0; i < n; i++) {
		if (rising ? table.at[i + 1] < table.at[i] : table.at[i + 1] > table.at[i]) {
			return nullptr;
		}
	}
	if (table.at[n] == table.at[0]) {
		return nullptr;
	}
	table.state = 1;
	return &table;
}
} // namespace

bool engineCellSetFine(int layer, int x, int y, float value) {
	InEngine inEngine;
	MenuItem* item = itemAt(layer, x, y);
	Editing editing(item, x);
	if (!editing.ok || dynamic_cast<Value<int32_t>*>(item) == nullptr || !editing.relevant(item, x)) {
		return false;
	}
	FineTable* table = fineTableOf(layer, x, y, item);
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* modelStack = table != nullptr ? paramOf(item, memory) : nullptr;
	if (modelStack == nullptr) {
		return false;
	}
	const float place = std::clamp(value, (float)table->min, (float)table->max) - (float)table->min;
	const int below = std::min((int)place, table->max - table->min - 1);
	const double fraction = (double)place - below;
	const double raw = (double)table->at[below] + ((double)table->at[below + 1] - (double)table->at[below]) * fraction;
	modelStack->autoParam->setCurrentValueInResponseToUserInput((int32_t)std::llround(raw), modelStack);
	return true;
}

bool engineCellGetFine(int layer, int x, int y, float* value) {
	InEngine inEngine;
	MenuItem* item = itemAt(layer, x, y);
	Editing editing(item, x);
	if (!editing.ok || dynamic_cast<Value<int32_t>*>(item) == nullptr || !editing.relevant(item, x)) {
		return false;
	}
	FineTable* table = fineTableOf(layer, x, y, item);
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* modelStack = table != nullptr ? paramOf(item, memory) : nullptr;
	if (modelStack == nullptr) {
		return false;
	}
	const int64_t raw = modelStack->autoParam->getCurrentValue();
	const int n = table->max - table->min;
	const bool rising = table->at[n] > table->at[0];
	int below = 0;
	while (below < n - 1 && (rising ? raw >= table->at[below + 1] : raw <= table->at[below + 1])) {
		below++;
	}
	const double span = (double)table->at[below + 1] - (double)table->at[below];
	const double fraction = span != 0.0 ? std::clamp(((double)raw - (double)table->at[below]) / span, 0.0, 1.0) : 0.0;
	*value = (float)(table->min + below + fraction);
	return true;
}

bool engineCellOption(int layer, int x, int y, int index, char* out, int outSize) {
	InEngine inEngine;
	if (outSize > 0) {
		out[0] = 0;
	}
	MenuItem* item = itemAt(layer, x, y);
	Editing editing(item, x);
	if (!editing.ok || index < 0 || outSize <= 0) {
		return false;
	}
	if (auto* selection = dynamic_cast<Selection*>(item)) {
		auto options = selection->getOptions();
		if ((size_t)index >= options.size()) {
			return false;
		}
		copy(out, (size_t)outSize, options[(size_t)index]);
		return true;
	}
	if (auto* choice = dynamic_cast<Enumeration*>(item)) {
		// An enumeration that makes its names up (sync levels): ask for each value's text.
		if ((size_t)index >= choice->size()) {
			return false;
		}
		choice->readCurrentValue();
		const int32_t saved = choice->getValue();
		choice->setValue(index);
		DEF_STACK_STRING_BUF(text, 40);
		item->getNotificationValue(text);
		choice->setValue(saved);
		copy(out, (size_t)outSize, text.c_str());
		return true;
	}
	return false;
}

bool engineCellTextFor(int layer, int x, int y, int32_t value, char* out, int outSize) {
	InEngine inEngine;
	if (outSize > 0) {
		out[0] = 0;
	}
	MenuItem* item = itemAt(layer, x, y);
	Editing editing(item, x);
	if (!editing.ok || outSize <= 0) {
		return false;
	}
	if (auto* toggle = dynamic_cast<deluge::gui::menu_item::Toggle*>(item)) {
		copy(out, (size_t)outSize, value != 0 ? "On" : "Off");
		return editing.relevant(item, x);
	}
	auto* holder = dynamic_cast<Value<int32_t>*>(item);
	if (holder == nullptr || !editing.relevant(item, x)) {
		return false;
	}
	// The item formats the value it holds; lend it this one and then let it read its own again.
	holder->readCurrentValue();
	holder->setValue(value);
	DEF_STACK_STRING_BUF(text, 40);
	item->getNotificationValue(text);
	holder->readCurrentValue();
	copy(out, (size_t)outSize, text.c_str());
	return true;
}

void engineGridRead(int32_t* values, uint8_t* relevant) {
	InEngine inEngine;
	std::memset(relevant, 0, kGridSlots);
	for (int layer = 0; layer < kGridLayers; layer++) {
		for (int x = 0; x < kGridColumns; x++) {
			for (int y = 0; y < kGridRows; y++) {
				MenuItem* item = itemAt(layer, x, y);
				if (item == nullptr) {
					continue;
				}
				auto* holder = dynamic_cast<Value<int32_t>*>(item);
				auto* toggle = holder == nullptr ? dynamic_cast<Value<bool>*>(item) : nullptr;
				if (holder == nullptr && toggle == nullptr) {
					continue;
				}
				Editing editing(item, x);
				if (!editing.ok) {
					return; // no preset: the same for every pad
				}
				if (!editing.relevant(item, x)) {
					continue;
				}
				const int slot = gridSlot(layer, x, y);
				item->readCurrentValue();
				values[slot] = holder != nullptr ? holder->getValue() : (toggle->getValue() ? 1 : 0);
				relevant[slot] = 1;
			}
		}
	}
}

} // namespace deluge_hosted
