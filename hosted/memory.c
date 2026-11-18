// See hosted.h. SPDX-License-Identifier: GPL-3.0-or-later
#include "hosted.h"

#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

uintptr_t deluge_hosted_external_begin;
uintptr_t deluge_hosted_external_end;
uintptr_t deluge_hosted_internal_begin;
uintptr_t deluge_hosted_internal_small_end;
uintptr_t deluge_hosted_internal_end;

// The Deluge has 64 MB of external RAM and streams samples through it. Here samples are kept
// whole in memory, so the region is larger. The pages are meant to be committed only as they are
// touched; see the end of deluge_hosted_memory_init() for what that takes inside an MPC.
#define EXTERNAL_SIZE (256u << 20)
#define INTERNAL_SMALL_SIZE (256u << 10)
#define INTERNAL_SIZE (4u << 20)

uintptr_t deluge_hosted_window;
int deluge_hosted_scrub = -1;

void deluge_hosted_memory_init(void) {
	if (deluge_hosted_external_begin != 0) {
		return;
	}
	const char* scrub = getenv("DELUGE_SCRUB");
	if (scrub != NULL && *scrub != 0) {
		deluge_hosted_scrub = atoi(scrub) & 0xFF;
	}
	const size_t total = EXTERNAL_SIZE + INTERNAL_SMALL_SIZE + INTERNAL_SIZE;
#if UINTPTR_MAX > 0xFFFFFFFFu
	// See hosted.h: the block must lie inside one 4 GB-aligned window. Reserve 8 GB of address
	// space (no memory), which is certain to contain a whole window, then give back everything
	// but the block. The block starts 256 MB into the window, where the Deluge's own memory map
	// has its RAM, so every 32-bit form is non-zero and positive as a signed number too.
	const uintptr_t window = (uintptr_t)1 << 32;
	const uintptr_t offset = 0x10000000u;
	char* reserved = mmap(NULL, 2 * window, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (reserved == MAP_FAILED) {
		abort();
	}
	const uintptr_t base = ((uintptr_t)reserved + window - 1) & ~(window - 1);
	char* block = (char*)(base + offset);
	if (block > reserved) {
		munmap(reserved, (size_t)(block - reserved));
	}
	munmap(block + total, (size_t)((reserved + 2 * window) - (block + total)));
	if (mprotect(block, total, PROT_READ | PROT_WRITE) != 0) {
		abort();
	}
	deluge_hosted_window = base;
#else
	void* block = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (block == MAP_FAILED) {
		abort();
	}
#endif
#ifdef __linux__
	// The MPC application locks its memory (the SDK's host notes say so, and a Force gave a low
	// memory warning with this plugin loaded, 2026-11-18). If it does so for mappings yet to come
	// (mlockall with MCL_FUTURE), the whole external region became real memory the moment it was
	// mapped: 256 MB for a plugin that mostly plays presets with no samples at all. Unlock that
	// region and hand its pages back; they return, zeroed, one at a time as they are first used.
	// The small internal regions stay as they are. Harmless where nothing was locked.
	munlock(block, EXTERNAL_SIZE);
	madvise(block, EXTERNAL_SIZE, MADV_DONTNEED);
#endif
	deluge_hosted_external_begin = (uintptr_t)block;
	deluge_hosted_external_end = deluge_hosted_external_begin + EXTERNAL_SIZE;
	deluge_hosted_internal_begin = deluge_hosted_external_end;
	deluge_hosted_internal_small_end = deluge_hosted_internal_begin + INTERNAL_SMALL_SIZE;
	deluge_hosted_internal_end = deluge_hosted_internal_small_end + INTERNAL_SIZE;
}

// How much of the block is in real memory now, and how big the block is, in megabytes. For
// diagnostics; walks the page table, so not for the audio thread.
int deluge_hosted_memory_resident_mb(int* totalMb) {
	const size_t total = EXTERNAL_SIZE + INTERNAL_SMALL_SIZE + INTERNAL_SIZE;
	if (totalMb != NULL) {
		*totalMb = (int)(total >> 20);
	}
	if (deluge_hosted_external_begin == 0) {
		return 0;
	}
	const size_t page = (size_t)sysconf(_SC_PAGESIZE);
	const size_t pages = total / page;
	static unsigned char seen[(EXTERNAL_SIZE + INTERNAL_SMALL_SIZE + INTERNAL_SIZE) / 4096];
	if (pages > sizeof seen || mincore((void*)deluge_hosted_external_begin, total, (void*)seen) != 0) {
		return -1;
	}
	size_t resident = 0;
	for (size_t i = 0; i < pages; i++) {
		resident += seen[i] & 1;
	}
	return (int)((resident * page) >> 20);
}
