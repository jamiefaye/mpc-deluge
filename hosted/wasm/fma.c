// WebAssembly build only. SPDX-License-Identifier: GPL-3.0-or-later
//
// On ARM the compiler turns "a * b + c" written in one floating-point expression into a single
// fused multiply-add instruction (one rounding instead of two); the firmware's reverb, compressor
// and DX7 code are full of them, and their output depends on it in the last bits. WebAssembly has
// no such instruction, so tools/build_firmware.py makes each of those places a call to fmaf() or
// fma() (see wasm_compile there). This is the fmaf() they reach: exact for every input, including
// results in the subnormal range, where the C library's version (musl) can be one bit out. fma()
// for doubles is the C library's, which works in integers and is exact.
#include <stdint.h>
#include <string.h>

float fmaf(float x, float y, float z) {
	// The product of two floats is exact in a double. The sum may not be: get its rounding error
	// exactly (Knuth's two-sum), and if there is one, move the sum to the neighbouring double with
	// an odd last bit on the side the true value lies ("round to odd"). Rounding that to float
	// then gives what rounding the true value would.
	const double xy = (double)x * (double)y;
	double s = xy + (double)z;
	if (s - s != 0.0) {
		return (float)s; // infinity or NaN
	}
	const double zPart = s - xy;
	const double xyPart = s - zPart;
	const double error = (xy - xyPart) + ((double)z - zPart);
	if (error != 0.0) {
		double sum = s;
		uint64_t bits;
		memcpy(&bits, &sum, sizeof bits);
		if ((bits & 1) == 0) {
			const int sumNegative = (int)(bits >> 63);
			bits += (sumNegative == (error < 0.0)) ? 1 : (uint64_t)-1; // away from zero : toward zero
			memcpy(&sum, &bits, sizeof sum);
		}
		return (float)sum;
	}
	return (float)s;
}
