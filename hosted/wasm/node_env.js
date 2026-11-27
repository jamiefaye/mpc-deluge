// Linked into the wasm test programs that run under Node (tools/build_firmware.py wasm --test):
// gives the program the real environment variables (DELUGE_SCRUB, DELUGE_LOG), which Emscripten
// otherwise replaces with a made-up set. SPDX-License-Identifier: GPL-3.0-or-later
Module['preRun'] = function() {
  // (A program that never asks for a variable has no ENV at all.)
  if (typeof ENV === 'object' && typeof process === 'object' && process.env) {
    for (var name in process.env) {
      ENV[name] = process.env[name];
    }
  }
};
