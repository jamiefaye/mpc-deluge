// engine_script as a function, for a WebAssembly module loaded by a web page or by Node: the same
// commands (see engine_script.cpp, which this includes whole and does not change), given as text
// instead of on standard input, and callable any number of times.
//
//   int run_script(const char* root, const char* commands)
//
// root is the folder presets and samples are found under (in the module's own file system when
// built as below; any path a "render" or "save" command names is in that file system too).
// Returns 0, or 1 if a command failed; the reason goes to standard error, which is the "printErr"
// function given to the module, and what the commands print goes to "print".
//
//   tools/build_firmware.py wasm --module tests/engine_module.cpp
//   -> build/firmware/wasm/engine_module.mjs and engine_module.wasm
//
//   import createEngine from "./engine_module.mjs";
//   const engine = await createEngine({print: line => ..., printErr: line => ...});
//   engine.FS.mkdirTree("/sd/SYNTHS");
//   engine.FS.writeFile("/sd/SYNTHS/A.XML", bytes);
//   const status = engine.ccall("run_script", "number", ["string", "string"],
//                               ["/sd", "load|SYNTHS/A.XML\non|60|127\nrender|689|/out.raw\n"]);
//   const audio = new Int32Array(engine.FS.readFile("/out.raw").buffer);   // interleaved stereo
//   engine.FS.unlink("/out.raw");                                          // render appends
//
// The engine is not reset between calls: free-running oscillators and LFOs, the noise generator
// and effect tails carry on from the call before, so the same script gives different audio the
// second time, as it does in engine_script when one run loads several presets. Where the audio
// must be what a new engine_script would produce, make a new instance for the script (a few
// milliseconds; compile the .wasm once and hand it to each through "instantiateWasm").
// tests/engine_module_check.mjs checks both uses against the native program.
// SPDX-License-Identifier: GPL-3.0-or-later

// Everything engine_script.cpp includes, first, so that the two names redefined below are
// redefined in its own text only.
#include "cables.h"
#include "crash_report.h"
#include "engine.h"
#include "matrix.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <strings.h>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

// engine_script's fail() ends the program with std::exit(1). Here that has to end the script and
// leave the module alive: its "exit" is this, which goes back to run_script().
namespace std {
[[noreturn]] void engine_script_stop(int status);
}
#define main engine_script_main
#define exit engine_script_stop
#include "engine_script.cpp"
#undef main
#undef exit

static std::jmp_buf stopped;
static int stoppedStatus;

[[noreturn]] void std::engine_script_stop(int status) {
	stoppedStatus = status;
	std::longjmp(stopped, 1); // past fail()'s callers; what their strings held is not given back
}

// (A function of its own, with nothing to destroy, around setjmp.)
static int runMain(char** arguments) {
	if (setjmp(stopped) != 0) {
		return stoppedStatus;
	}
	return engine_script_main(2, arguments);
}

extern "C" EMSCRIPTEN_KEEPALIVE int run_script(const char* root, const char* commands) {
	std::istringstream text{std::string(commands)};
	std::streambuf* const standardInput = std::cin.rdbuf(text.rdbuf());
	std::cin.clear();
	lineNumber = 0;
	char name[] = "engine_script";
	char* arguments[] = {name, const_cast<char*>(root), nullptr};
	const int status = runMain(arguments);
	std::cin.rdbuf(standardInput);
	std::fflush(stdout);
	std::fflush(stderr);
	return status;
}
