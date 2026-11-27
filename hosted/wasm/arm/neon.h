// WebAssembly build only (tools/build_firmware.py adds this folder to the include path for the
// wasm target). Argon, off ARM, includes SIMDe as <arm/neon.h>. Emscripten ships SIMDe's NEON
// implementation, which compiles to WebAssembly SIMD, under the name <arm_neon.h>, with the NEON
// names already aliased. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <arm_neon.h>
