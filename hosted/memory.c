// See hosted.h. SPDX-License-Identifier: GPL-3.0-or-later
#include "hosted.h"

#include <stdlib.h>
#include <sys/mman.h>

uintptr_t deluge_hosted_external_begin;
uintptr_t deluge_hosted_external_end;
uintptr_t deluge_hosted_internal_begin;
uintptr_t deluge_hosted_internal_small_end;
uintptr_t deluge_hosted_internal_end;

// The Deluge has 64 MB of external RAM and streams samples through it. Here samples are kept
// whole in memory, so the region is larger. The pages are only committed as they are touched.
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
	void* block = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (block == MAP_FAILED) {
		abort();
	}
#endif
	deluge_hosted_external_begin = (uintptr_t)block;
	deluge_hosted_external_end = deluge_hosted_external_begin + EXTERNAL_SIZE;
	deluge_hosted_internal_begin = deluge_hosted_external_end;
	deluge_hosted_internal_small_end = deluge_hosted_internal_begin + INTERNAL_SMALL_SIZE;
	deluge_hosted_internal_end = deluge_hosted_internal_small_end + INTERNAL_SIZE;
}
