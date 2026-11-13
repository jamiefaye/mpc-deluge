// Hosted hardware layer: what stands in for the Deluge's hardware when the firmware runs inside
// an MPC plugin. Included from patched firmware headers, so keep it plain C.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- memory ------------------------------------------------------------------------------------
// The firmware's allocator works on address ranges fixed by the Deluge's memory map: 64 MB of
// external SDRAM and a few MB of internal RAM above it. Here both come from one block obtained
// from the system, laid out external first, then internal, because some firmware code tells the
// two apart by comparing addresses. The block is created on first use.
extern uintptr_t deluge_hosted_external_begin;       // also where the "stealable" region starts
extern uintptr_t deluge_hosted_external_end;         // == deluge_hosted_internal_begin
extern uintptr_t deluge_hosted_internal_begin;       // start of the small internal region
extern uintptr_t deluge_hosted_internal_small_end;   // end of small, start of general internal
extern uintptr_t deluge_hosted_internal_end;
void deluge_hosted_memory_init(void);

// 64-bit targets. The firmware keeps addresses in 32-bit fields in many places: the allocator's
// free list, and sorted arrays and hash tables keyed by "the address of this object". Rather than
// widen every one of those (which would change the layout of structures the engine's behaviour
// depends on), the whole block above sits inside one 4 GB-aligned window of the address space,
// well clear of the window's start. Then the low 32 bits of any address in the block identify it
// uniquely, keep their order, and are never 0, so
//   pointer -> 32 bits   is plain truncation, (uint32_t)(uintptr_t)p, and
//   32 bits -> pointer   is deluge_hosted_ptr(), which puts the window's top half back.
// This holds only for memory from the firmware's allocator, which is where every object the
// firmware uses this way lives (its operator new goes there too). Turning an integer into a
// pointer any other way is a compile error on 64-bit (see tools/build_firmware.py).
// On 32-bit targets both directions are the identity and nothing changes.
extern uintptr_t deluge_hosted_window; // top 32 bits of every address in the block; 0 on 32-bit
static inline void* deluge_hosted_ptr(uint32_t low) {
#if UINTPTR_MAX > 0xFFFFFFFFu
	return low ? (void*)(deluge_hosted_window | low) : (void*)0;
#else
	return (void*)low;
#endif
}

// Test aid. Firmware code sometimes reads memory it has just been given before writing it, and
// what it finds there depends on what was freed earlier, which differs between a 32-bit and a
// 64-bit build because their objects differ in size. With the environment variable DELUGE_SCRUB
// set to a byte value (0-255), every block the allocator hands out is filled with that byte
// first, so such reads give the same thing everywhere and two builds can be compared exactly.
// -1 (the default) leaves memory alone, as on a Deluge.
extern int deluge_hosted_scrub;

// --- audio input -------------------------------------------------------------------------------
// The Deluge can use its audio input as an oscillator. The plugin is an instrument with no input,
// so those oscillators are silent (model/voice/voice.cpp). Wiring an input up later means feeding
// the firmware's receive buffer every block and dealing with what that code does on digital
// silence, which a Deluge's converter never produces; see CLAUDE.md, "Audio input".
#define DELUGE_HOSTED_AUDIO_INPUT 0

// --- files -------------------------------------------------------------------------------------
// The firmware's paths ("SYNTHS/Foo.XML") are resolved under this folder. No trailing slash.
void deluge_hosted_set_root(const char* folder);

// --- time --------------------------------------------------------------------------------------
// The firmware reads free-running 16-bit hardware timers. They advance only when told to: call
// this with the number of audio samples just rendered.
void deluge_hosted_advance_samples(uint32_t numSamples);

// --- log ---------------------------------------------------------------------------------------
// Text the firmware would send to its debug UART. Dropped unless the environment variable
// DELUGE_LOG is set, then written to stderr.
void deluge_hosted_log(const char* text);

#ifdef __cplusplus
}
// Called on entering and leaving each of the engine's functions (hosted/engine.h). While a thread
// is between the two, operator new on it uses the firmware's allocator; see hosted/operators.cpp.
// (Functions rather than a shared thread-local variable: one of those is reached through a helper
// symbol that a plugin would have to import from the MPC, which does not have it.)
void deluge_hosted_enter_engine() noexcept;
void deluge_hosted_leave_engine() noexcept;
#endif
