// The loaded sound's patch cables (the Deluge's "mod matrix"): which source moves which
// parameter, and by how much. Read and written the way the firmware's own cable-strength menu
// does it (gui/menu_item/patch_cable_strength.cpp), so scaling, creation and removal are its own.
//
// A cable runs from a source to a parameter. A second kind runs from a source to another cable's
// depth (the mod wheel setting how much vibrato LFO1 gives): `of` is then that other cable's
// source, and `destination` its parameter. A cable exists only while its amount is not zero.
//
// Same rules as engine.h: the caller serialises every call; these act on the current instance.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace deluge_hosted {

constexpr int kMaxCables = 32;        // the firmware's limit for one sound
constexpr int kCableAmountMax = 5000; // amounts run -5000..5000: the firmware shows them as -50.00..50.00

struct Cable {
	int16_t source;      // the firmware's source number (engineCableSourceName)
	int16_t destination; // the firmware's parameter number (engineCableDestinationName)
	int16_t of;          // -1, or the source of the cable whose depth this one moves
	int32_t amount;
};

// Every cable of the sound, in the firmware's order. Returns how many (at most `max` are written).
int engineCableList(Cable* out, int max);
// Sets a cable's amount: makes the cable if there is none, removes it at zero. Does nothing if
// the sound already has kMaxCables. Any source may be put on any parameter, as in a preset file;
// some do nothing (a per-voice source on a parameter the whole sound shares, a filter that is off).
void engineCableSet(int source, int destination, int of, int32_t amount);

// The sources and destinations there are, for building tables: numbers run from 0, a name is
// empty for a number that is not a patchable thing. Return false past the end.
bool engineCableSourceName(int source, char* out, int outSize);
bool engineCableDestinationName(int destination, char* out, int outSize);

} // namespace deluge_hosted
