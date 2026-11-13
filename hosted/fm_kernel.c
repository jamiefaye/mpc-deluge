// The DX7 engine's operator kernel in portable C. SPDX-License-Identifier: GPL-3.0-or-later
//
// The firmware has this routine only as 32-bit ARM NEON assembly (dsp/dx/neon_fm_kernel.s, from
// Raph Levien's music-synthesizer-for-android). It does not compute the sine the way the
// firmware's scalar code does: it evaluates a polynomial in single-precision floating point, and
// it ramps the gain in floating point too. To sound the same on a 64-bit MPC this file repeats
// those operations one for one, in the same order and precision, so the result is bit-identical
// (tests/fm_kernel_check.cpp compares the two on 32-bit ARM).
//
// What the assembly does, per sample i (vector lane j = i % 4, vector number k = i / 4):
//   phase  = phase0 + i * freq                      32-bit wrap-around
//   x      = in[i] + phase
//   t      = (x & 0x7fffff) - 0x400000              position within a half cycle
//   f      = t / 2^22 as float, f2 = f * f
//   s      = ((c0 * f2 + c1) * f2 + c2) * f2 + c3   each multiply and each add rounded separately
//   g      = gain of lane j: starts as float(gain + j * dgain) / 2^24, and is stepped by
//            float(dgain) / 2^22 once per vector, in floating point
//   v      = s * g, times 2^24, truncated to int32 with saturation
//   out[i] = (x has bit 23 set ? ~v : v) + busin[i]
#include <stdint.h>

// Each multiply and add must round on its own, as the NEON instructions do (vmla is not fused).
#pragma STDC FP_CONTRACT OFF

static inline int32_t to_fixed24(float value) {
	float scaled = value * 16777216.0f;
	if (scaled != scaled) {
		return 0;
	}
	if (scaled >= 2147483648.0f) {
		return INT32_MAX;
	}
	if (scaled <= -2147483648.0f) {
		return INT32_MIN;
	}
	return (int32_t)scaled; // toward zero, like vcvt
}

void deluge_hosted_fm_kernel(const int32_t* in, const int32_t* busin, int32_t* out, int count, int32_t phase0,
                             int32_t freq, int32_t gain1, int32_t dgain) {
	const float c0 = -0.01880853017455781f, c1 = 0.25215252666796095f, c2 = -1.2333439964934032f, c3 = 1.0f;
	float gain[4];
	for (int j = 0; j < 4; j++) {
		gain[j] = (float)(int32_t)((uint32_t)gain1 + (uint32_t)dgain * (uint32_t)j) * (1.0f / 16777216.0f);
	}
	const float gainStep = (float)dgain * (1.0f / 4194304.0f);
	uint32_t phase = (uint32_t)phase0;
	for (int i = 0; i < count; i++) {
		const int j = i & 3;
		const uint32_t x = (uint32_t)in[i] + phase;
		phase += (uint32_t)freq;
		const float f = (float)((int32_t)(x & 0x7fffff) - 0x400000) * (1.0f / 4194304.0f);
		const float f2 = f * f;
		float s = f2 * c0;
		s = c1 + s;
		s = f2 * s;
		s = c2 + s;
		s = f2 * s;
		s = c3 + s;
		const float product = s * gain[j];
		gain[j] = gain[j] + gainStep;
		int32_t v = to_fixed24(product);
		if (x & 0x800000) {
			v = ~v;
		}
		out[i] = (int32_t)((uint32_t)v + (uint32_t)busin[i]);
	}
}

#if !defined(__arm__)
void neon_fm_kernel(const int32_t* in, const int32_t* busin, int32_t* out, int count, int32_t phase0, int32_t freq,
                    int32_t gain1, int32_t dgain) {
	deluge_hosted_fm_kernel(in, busin, out, count, phase0, freq, gain1, dgain);
}
#endif
