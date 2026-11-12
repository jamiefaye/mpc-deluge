// Run this library's static destructors when it is unloaded.
//
// A C++ static object with a destructor is registered with __cxa_atexit, tagged with the library
// it belongs to. Normally the compiler's start-up files (crtbeginS.o) give every shared library
// a finaliser that calls __cxa_finalize for its own tag, so those destructors run at dlclose and
// are forgotten. A library linked by `zig c++` has no such finaliser: the registrations outlive
// the library, and when the host process later exits it calls destructors at addresses that are no
// longer mapped. Seen as a SIGSEGV at exit in the emulated arm32 run of any plugin that used
// mpc::pluginFolder() (it keeps a static std::string). On an MPC that would be a crash whenever
// the MPC application shuts down after the plugin was unloaded.
//
// tools/build.py adds this file to every plugin built in this repository.
//
// SPDX-License-Identifier: MIT

extern "C" {
void __cxa_finalize(void* dso);
extern void* __dso_handle __attribute__((visibility("hidden")));
}

namespace {
__attribute__((destructor)) void runStaticDestructorsOnUnload() {
	__cxa_finalize(&__dso_handle);
}
} // namespace
