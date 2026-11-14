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

# The SDK's generator gives every ui.json page a function key of its own, but an MPC has six keys.
# A project's tabs.json says which pages share a key (pressing the key again steps through them)
# and which sixteen parameters the Q-Links control in the MPC's Program mode. The generated files
# are changed as text, not parsed and rewritten: the MPC's parser depends on key order, and
# TUI.json repeats a key in places, which a JSON library would drop.
import json  # noqa: E402
import re  # noqa: E402


def apply_tabs(project, out_dir, params_json):
    spec_path = os.path.join(project, "tabs.json")
    tui_path = os.path.join(out_dir, "Plugin Skins", "TUI.json")
    q_path = os.path.join(out_dir, "Plugin Skins", "Q-Links.json")
    if not (os.path.isfile(spec_path) and os.path.isfile(tui_path) and os.path.isfile(q_path)):
        return
    spec = json.load(open(spec_path))
    place = {}  # page name -> (function key, position on it), both from 0
    for key, names in enumerate(spec["keys"]):
        if len(names) > 8:
            raise sdk_build.BuildError("tabs.json: more than 8 pages on function key %d" % (key + 1))
        for sub, name in enumerate(names):
            place[name] = (key, sub)
    if len(spec["keys"]) > 6:
        raise sdk_build.BuildError("tabs.json: more than six function keys")

    tui = open(tui_path, encoding="utf-8").read()
    tab = re.compile(r'("tabName"\s*:\s*"([^"]*)"\s*,\s*"fnKeyIndex"\s*:\s*)(\d+)(\s*,\s*"fnKeySubIndex"\s*:\s*)(\d+)')
    order = []  # the generator's tab number (from 1) -> where it goes

    def move(m):
        if m.group(2) not in place:
            raise sdk_build.BuildError("tabs.json does not place the page \"%s\"" % m.group(2))
        key, sub = place[m.group(2)]
        order.append((key, sub))
        return "%s%d%s%d" % (m.group(1), key, m.group(4), sub)

    tui = tab.sub(move, tui)
    if len(order) != len(place):
        raise sdk_build.BuildError("tabs.json names %d pages, the skin has %d" % (len(place), len(order)))
    open(tui_path, "w", encoding="utf-8").write(tui)

    q = open(q_path, encoding="utf-8").read()
    entry = re.compile(r'("Tab"\s*:\s*)(\d+)(\s*,\s*"SubTab"\s*:\s*)(\d+)')
    q, n = entry.subn(lambda m: "%s%d%s%d" % (m.group(1), order[int(m.group(2)) - 1][0] + 1, m.group(3),
                                              order[int(m.group(2)) - 1][1] + 1), q)
    if n != len(order):
        raise sdk_build.BuildError("Q-Links.json has %d tab entries, expected %d" % (n, len(order)))
    if "programQLinks" in spec:
        index = {p["name"]: p["index"] for p in json.load(open(params_json))["params"]}
        head, sep, tail = q.partition('"Program Mode Q-Links"')
        for knob, name in enumerate(spec["programQLinks"][:16]):
            if name not in index:
                raise sdk_build.BuildError("tabs.json programQLinks: no parameter called \"%s\"" % name)
            tail, n = re.subn(r'("Q-Link %d"\s*:\s*)-?\d+' % (knob + 1), lambda m: m.group(1) + str(index[name]), tail, count=1)
            if n != 1:
                raise sdk_build.BuildError("Q-Links.json: no Program Mode entry for Q-Link %d" % (knob + 1))
        q = head + sep + tail
    open(q_path, "w", encoding="utf-8").write(q)
    sdk_build.log("  tabs.json: %d pages on %d function keys" % (len(order), len(spec["keys"])))


_make_skins = sdk_build.make_skins


def make_skins(project, out_dir, params_json, no_ui):
    made = _make_skins(project, out_dir, params_json, no_ui)
    if made:
        apply_tabs(project, out_dir, params_json)
    return made


sdk_build.make_skins = make_skins

if __name__ == "__main__":
    sys.exit(sdk_build.main())
