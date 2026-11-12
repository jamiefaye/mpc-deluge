// Check the firmware's fixed-point primitives (util/intrinsics.h) against plain 64-bit arithmetic.
// On 32-bit ARM they are inline assembly written for GCC; this build uses clang.
//   tools/build_firmware.py arm32 --test tests/intrinsics_check.cpp
//   tools/armtest/run.py --exe build/firmware/arm32/intrinsics_check
// SPDX-License-Identifier: GPL-3.0-or-later
#include "util/fixedpoint.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>

static uint32_t rngState = 12345;
static int32_t rnd() {
	rngState = rngState * 1664525u + 1013904223u;
	uint32_t v = rngState;
	// mix magnitudes so small and extreme values both turn up
	int shift = (rngState >> 27) & 31;
	return (int32_t)v >> (shift % 28);
}
static int64_t clamp64(int64_t v, int64_t lo, int64_t hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

static int failures = 0;
#define CHECK(name, got, want, a, b)                                                                                   \
	do {                                                                                                               \
		if ((int64_t)(got) != (int64_t)(want)) {                                                                       \
			if (failures < 40) {                                                                                       \
				std::printf("FAIL %s(%d, %d) = %lld, want %lld\n", name, (int)(a), (int)(b), (long long)(got),         \
				            (long long)(want));                                                                        \
			}                                                                                                          \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

int main() {
	const int32_t edge[] = {0, 1, -1, 2, -2, 0x7FFFFFFF, (int32_t)0x80000000, 0x40000000, (int32_t)0xC0000000, 0x00FFFFFF,
	                        (int32_t)0xFF000000, 8388607, -8388608, 8388608, -8388609};
	for (int i = 0; i < 200000; i++) {
		int32_t a = i < 15 * 15 ? edge[i / 15] : rnd();
		int32_t b = i < 15 * 15 ? edge[i % 15] : rnd();
		int32_t s = rnd();
		volatile int32_t va = a, vb = b, vs = s; // keep the compiler from folding the calls away
		CHECK("multiply_32x32_rshift32", multiply_32x32_rshift32(va, vb), ((int64_t)a * b) >> 32, a, b);
		CHECK("multiply_32x32_rshift32_rounded", multiply_32x32_rshift32_rounded(va, vb),
		      (int32_t)(((int64_t)a * b + 0x80000000LL) >> 32), a, b);
		CHECK("multiply_accumulate_32x32_rshift32", multiply_accumulate_32x32_rshift32(vs, va, vb),
		      (int32_t)((((int64_t)s << 32) + (int64_t)a * b) >> 32), a, b);
		CHECK("multiply_accumulate_32x32_rshift32_rounded", multiply_accumulate_32x32_rshift32_rounded(vs, va, vb),
		      (int32_t)((((int64_t)s << 32) + (int64_t)a * b + 0x80000000LL) >> 32), a, b);
		CHECK("multiply_subtract_32x32_rshift32_rounded", multiply_subtract_32x32_rshift32_rounded(vs, va, vb),
		      (int32_t)((((int64_t)s << 32) - (int64_t)a * b + 0x80000000LL) >> 32), a, b);
		CHECK("signed_saturate<24>", signed_saturate<24>(va), clamp64(a, -(1LL << 23), (1LL << 23) - 1), a, 0);
		CHECK("signed_saturate<16>", signed_saturate<16>(va), clamp64(a, -(1LL << 15), (1LL << 15) - 1), a, 0);
		CHECK("signed_saturate<31>", signed_saturate<31>(va), clamp64(a, -(1LL << 30), (1LL << 30) - 1), a, 0);
		CHECK("unsigned_saturate<16>", (uint32_t)unsigned_saturate<16>((uint32_t)va),
		      (int64_t)((int64_t)a < 0 ? 0 : clamp64(a, 0, 65535)), a, 0);
		// shift_left_saturate<> is not checked: no firmware code calls it, and on ARM it does not
		// saturate at 32 bits (SSAT shifts in 32 bits first), unlike its portable fallback.
		CHECK("add_saturate", add_saturate(va, vb), clamp64((int64_t)a + b, INT32_MIN, INT32_MAX), a, b);
		CHECK("subtract_saturate", subtract_saturate(va, vb), clamp64((int64_t)a - b, INT32_MIN, INT32_MAX), a, b);
		// float <-> q31
		float f = (float)a / 2147483648.0f * 1.25f; // sometimes just outside -1..1
		volatile float vf = f;
		double want = std::floor((double)f * 2147483648.0); // vcvt truncates toward zero
		want = (double)f >= 0 ? want : -std::floor(-(double)f * 2147483648.0);
		want = want > 2147483647.0 ? 2147483647.0 : want < -2147483648.0 ? -2147483648.0 : want;
		CHECK("q31_from_float", q31_from_float(vf), (int64_t)want, a, 0);
		float back = int32_to_float(va);
		float wantBack = (float)((double)a / 2147483648.0);
		if (back != wantBack) {
			if (failures < 40) {
				std::printf("FAIL int32_to_float(%d) = %.9g, want %.9g\n", (int)a, (double)back, (double)wantBack);
			}
			failures++;
		}
	}
	std::printf("intrinsics check: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
