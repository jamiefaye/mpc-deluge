#!/usr/bin/env python3
"""Build a plugin with the MPC Plugin SDK's build.py, adjusted so the plugin can compile Deluge
firmware source from third_party/ (run tools/vendor.sh first).

The SDK script compiles src/*.cpp in one command as C++17. The firmware needs C++23 and its own
include paths, so this wrapper changes the SDK's flag list and then hands over to it. Firmware
headers and sources are reached through -isystem, which also keeps their warnings out of the log.

    python3 tools/build.py plugins/<name> [any build.py option]
"""
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.environ.get("MPC_SDK", os.path.join(HERE, "..", "mpc-plugin-builder-sdk"))
TP = os.path.join(HERE, "third_party")

sys.path.insert(0, os.path.join(SDK, "tools"))
import build as sdk_build  # noqa: E402

flags = sdk_build.COMMON_FLAGS
flags[flags.index("-std=c++17")] = "-std=c++23"
flags += ["-DDELUGE_HOSTED=1"]
# What the firmware's own CMake build passes and its code relies on. char is unsigned by default
# on ARM; the firmware is written for signed char.
flags += ["-fsigned-char", "-D_GNU_SOURCE", "-DUSE_TASK_MANAGER"]
# hosted/hosted.h is included by the patched firmware headers.
flags += ["-I" + os.path.join(HERE, "hosted")]
for inc in ("deluge/src", "deluge/src/deluge", "argon/include", "etl/include"):
    path = os.path.join(TP, inc)
    if not os.path.isdir(path):
        sys.exit("missing %s: run tools/vendor.sh" % path)
    flags += ["-isystem", path]

# Every plugin also gets common/*.cpp (see the comments in those files for why).
COMMON_SOURCES = sorted(
    os.path.join(HERE, "common", f) for f in os.listdir(os.path.join(HERE, "common")) if f.endswith(".cpp"))
_find_sources = sdk_build.find_sources
sdk_build.find_sources = lambda project: _find_sources(project) + COMMON_SOURCES

# A plugin whose folder holds a file called "link-firmware" gets the firmware library on its link
# line: build/firmware/<target>/libdeluge.a for arm32, arm64 and, on a 64-bit ARM computer, the
# native build (tools/build_firmware.py makes them; build.sh calls it first). Those builds are
# compiled with DELUGE_ENGINE_LINKED defined. A build without the library (the native build on an
# Intel computer, where the firmware's NEON code cannot be compiled, and the desktop VST) has to
# supply the engine itself; see plugins/deluge-synth/src/engine_stub.cpp.
import platform  # noqa: E402

NATIVE_IS_ARM64 = platform.machine().lower() in ("arm64", "aarch64")


def firmware_lib(target):
    return os.path.join(HERE, "build", "firmware", target, "libdeluge.a")


def with_firmware(self, target, build):
    """Run build() with the firmware library for target added to the link, if this plugin wants it."""
    if not os.path.exists(os.path.join(self.project, "link-firmware")):
        return build()
    if target == "native" and not NATIVE_IS_ARM64:
        return build()
    lib = firmware_lib(target)
    if not os.path.exists(lib):
        raise sdk_build.BuildError("this plugin links the firmware: run tools/build_firmware.py %s first" % target)
    saved = self.sources
    self.sources = saved + [lib, "-DDELUGE_ENGINE_LINKED=1", "-lpthread"] + ([] if sys.platform == "darwin" else ["-ldl"])
    try:
        return build()
    finally:
        self.sources = saved


_build_arm = sdk_build.Builder.build_arm
_build_native = sdk_build.Builder.build_native
sdk_build.Builder.build_arm = lambda self, target, output: with_firmware(
    self, target, lambda: _build_arm(self, target, output))
sdk_build.Builder.build_native = lambda self, output: with_firmware(self, "native", lambda: _build_native(self, output))

if __name__ == "__main__":
    sys.exit(sdk_build.main())
