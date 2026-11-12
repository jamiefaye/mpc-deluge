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

void deluge_hosted_memory_init(void) {
	if (deluge_hosted_external_begin != 0) {
		return;
	}
	const size_t total = EXTERNAL_SIZE + INTERNAL_SMALL_SIZE + INTERNAL_SIZE;
	void* block = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (block == MAP_FAILED) {
		abort();
	}
	deluge_hosted_external_begin = (uintptr_t)block;
	deluge_hosted_external_end = deluge_hosted_external_begin + EXTERNAL_SIZE;
	deluge_hosted_internal_begin = deluge_hosted_external_end;
	deluge_hosted_internal_small_end = deluge_hosted_internal_begin + INTERNAL_SMALL_SIZE;
	deluge_hosted_internal_end = deluge_hosted_internal_small_end + INTERNAL_SIZE;
}
