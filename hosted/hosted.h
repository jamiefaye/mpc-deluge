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
#endif
