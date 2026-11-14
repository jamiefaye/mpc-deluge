// The Deluge's shortcut grid as a host program sees it: every pad of the 16 x 8 grid that edits a
// parameter of the loaded SYNTH preset, read and written through the firmware's own menu items
// (gui/ui/menus.cpp, paramShortcutsForSounds and its second layer), so ranges, scalings, value
// text and side effects are the firmware's.
//
// Same rules as engine.h: one engine per process, the caller serialises every call.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace deluge_hosted {

constexpr int kGridColumns = 16; // x, left to right
constexpr int kGridRows = 8;     // y, from the bottom, as the firmware numbers them
constexpr int kGridLayers = 2;   // 0: first press; 1: second press of the same pad

enum class CellKind : uint8_t {
	None,   // no shortcut on this pad, or one that is not a value (a file browser, a submenu)
	Number, // min..max in steps of 1
	Choice, // 0..max, each with a name (engineCellOption)
	Toggle, // 0 or 1
};

struct CellInfo {
	CellKind kind;
	bool relevant;   // false: the pad exists but does nothing in this preset (an FM control in a
	                 // subtractive sound, a sample control with no sample); value is then 0
	int32_t min, max, value;
	char name[48];   // the firmware's name for the item ("Attack", "LPF frequency")
	char title[48];  // its menu title, usually fuller ("Env1 attack")
	char text[32];   // the value as the firmware would show it
	char type[64];   // the firmware class of the menu item, for diagnostics
};

// Describes one pad for the loaded preset. Returns false if there is no preset or no such pad.
bool engineCellInfo(int layer, int x, int y, CellInfo* out);
// Sets a pad's value (clamped to its range). Does nothing for a pad that is not relevant.
void engineCellSet(int layer, int x, int y, int32_t value);
// For a Choice: the name of option `index`. Returns false past the end.
bool engineCellOption(int layer, int x, int y, int index, char* out, int outSize);
// The text the firmware would show for `value` on this pad, without setting it. Returns false if
// the pad is not relevant.
bool engineCellTextFor(int layer, int x, int y, int32_t value, char* out, int outSize);

// Every pad at once, cheaply (no strings): for use on the audio thread after a change, to see
// what else moved. Slot = layer * 128 + x * 8 + y. relevant[slot] is 0 for a pad that has no
// value or does nothing in this preset; values[slot] is then left alone.
constexpr int kGridSlots = kGridLayers * kGridColumns * kGridRows;
constexpr int gridSlot(int layer, int x, int y) { return layer * kGridColumns * kGridRows + x * kGridRows + y; }
void engineGridRead(int32_t* values, uint8_t* relevant);

} // namespace deluge_hosted
