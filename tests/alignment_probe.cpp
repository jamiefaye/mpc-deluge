// Does the emulated machine raise alignment faults for NEON accesses that demand alignment, as a
// real 32-bit ARM CPU does? Performs one such load from a deliberately misaligned address.
// A real Cortex-A17 dies with SIGBUS here. Run: tools/armtest/run.py --exe <built program>
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>

int main() {
	static uint32_t data[16] __attribute__((aligned(16))) = {1, 2, 3, 4, 5, 6, 7, 8};
	const char* misaligned = reinterpret_cast<const char*>(data) + 2;
	uint32_t out[4] __attribute__((aligned(16)));
	std::printf("about to do an aligned-only NEON load from a misaligned address\n");
	std::fflush(stdout);
#if defined(__arm__)
	__asm__ volatile("vld1.32 {d0, d1}, [%0 :128]\n vst1.32 {d0, d1}, [%1]" : : "r"(misaligned), "r"(out) : "d0", "d1", "memory");
#endif
	std::printf("NO FAULT: the emulator does not check NEON alignment (%u)\n", (unsigned)out[0]);
	return 0;
}
