// Compare the C form of the DX7 operator kernel (hosted/fm_kernel.c) with the firmware's 32-bit
// ARM assembly, bit for bit, over random inputs. Only meaningful on 32-bit ARM, where the
// assembly exists; elsewhere it prints a checksum of the C form to compare across targets.
//   tools/build_firmware.py arm32 --test tests/fm_kernel_check.cpp
//   tools/armtest/run.py --exe build/firmware/arm32/fm_kernel_check
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" void neon_fm_kernel(const int32_t* in, const int32_t* busin, int32_t* out, int count, int32_t phase0,
                               int32_t freq, int32_t gain1, int32_t dgain);
extern "C" void deluge_hosted_fm_kernel(const int32_t* in, const int32_t* busin, int32_t* out, int count,
                                        int32_t phase0, int32_t freq, int32_t gain1, int32_t dgain);

static uint32_t state = 2463534242u;
static uint32_t rnd() {
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}
// values of every magnitude, both signs
static int32_t rndWide() {
	return (int32_t)rnd() >> (rnd() % 30);
}

int main() {
	alignas(16) static int32_t in[64], bus[64], a[64 + 8], b[64 + 8];
	const int sizes[] = {12, 16, 24, 28, 36, 40, 48, 52, 60, 64}; // n mod 12 is 0 or 4, as the assembly requires
	uint64_t checksum = 1469598103934665603ull;
	long mismatches = 0, cases = 0;
	for (int trial = 0; trial < 200000; trial++) {
		const int n = sizes[rnd() % 10];
		const int kind = rnd() % 4;
		for (int i = 0; i < 64; i++) {
			in[i] = kind == 0 ? 0 : rndWide();
			bus[i] = kind == 1 ? 0 : rndWide();
		}
		const int32_t phase0 = (int32_t)rnd(), freq = rndWide();
		// gains as the engine makes them (up to about 2^26) most of the time, anything at all otherwise
		const int32_t gain1 = kind == 3 ? rndWide() : (int32_t)(rnd() >> 6), dgain = kind == 3 ? rndWide() : rndWide() >> 8;
		std::memset(a, 0x55, sizeof a);
		std::memset(b, 0x55, sizeof b);
		deluge_hosted_fm_kernel(in, bus, a, n, phase0, freq, gain1, dgain);
		for (int i = 0; i < n; i++) {
			checksum = (checksum ^ (uint32_t)a[i]) * 1099511628211ull;
		}
#if defined(__arm__)
		neon_fm_kernel(in, bus, b, n, phase0, freq, gain1, dgain);
		cases++;
		if (std::memcmp(a, b, sizeof(int32_t) * (size_t)n) != 0) {
			if (mismatches < 10) {
				for (int i = 0; i < n; i++) {
					if (a[i] != b[i]) {
						std::printf("MISMATCH trial %d n %d i %d: C %d asm %d (phase0 %d freq %d gain %d dgain %d in %d bus %d)\n",
						            trial, n, i, a[i], b[i], phase0, freq, gain1, dgain, in[i], bus[i]);
						break;
					}
				}
			}
			mismatches++;
		}
#endif
	}
#if defined(__arm__)
	std::printf("fm_kernel_check: %ld cases against the assembly, %ld differ\n", cases, mismatches);
#endif
	std::printf("fm_kernel_check: checksum of the C form %016llx\n", (unsigned long long)checksum);
	return mismatches ? 1 : 0;
}
