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


def matching(text, start):
    """The index just past the bracket that closes the one at text[start] (strings respected)."""
    depth, i, in_string = 0, start, False
    while i < len(text):
        c = text[i]
        if in_string:
            if c == "\\":
                i += 1
            elif c == '"':
                in_string = False
        elif c == '"':
            in_string = True
        elif c in "{[":
            depth += 1
        elif c in "}]":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise sdk_build.BuildError("TUI.json: unbalanced brackets")


def apply_buttons(project, out_dir, params_json):
    """tabs.json "buttons": bool parameters shown as a picture button instead of the kit's toggle
    with its name and value text, for instance the arrows that step through presets. Each entry
    is  "<parameter name>": {"image": "<name>", "bounds": "x y w h"}  with <name>_on.png and
    <name>_off.png in the project's skin-images/ folder, drawn 12 px smaller than the bounds each
    way. Bounds are in the page's pixels, so two buttons can share one cell of the generator's
    grid. The toggle's definition is copied without its text and with these images; the
    parameter's place on the page is pointed at the copy and moved."""
    spec_path = os.path.join(project, "tabs.json")
    tui_path = os.path.join(out_dir, "Plugin Skins", "TUI.json")
    if not (os.path.isfile(spec_path) and os.path.isfile(tui_path)):
        return
    spec = json.load(open(spec_path))
    buttons = spec.get("buttons") or {}
    moved = spec.get("place") or {}
    tui = open(tui_path, encoding="utf-8").read()
    # The captions of segment buttons ("Subtractive | Fm | Ringmod"). The generator writes each
    # as a label with a data handle it never connects and "hide when the data is invalid", and an
    # MPC then hides them all (seen on a Force, 2026-11-16: the buttons had no words). Shown
    # always instead; and a caption too long for its segment gets a smaller font.
    caption = re.compile(r'"name"\s*:\s*"[^"]* seg \d+"\s*,\s*"type"\s*:\s*"Label"')
    out, at, fixed = [], 0, 0
    for m in caption.finditer(tui):
        end = tui.index("}", tui.index('"boundsType"', m.end()))
        item = tui[m.start():end]
        text = re.search(r'"text"\s*:\s*"([^"]*)"', item).group(1)
        width = int(re.search(r'"bounds"\s*:\s*"\d+ \d+ (\d+) \d+"', item).group(1))
        points = min(22.0, max(13.0, (width - 6) / (0.46 * max(1, len(text)))))
        item = re.sub(r'("height"\s*:\s*)[0-9.]+', lambda h: "%s%.1f" % (h.group(1), points), item, count=1)
        item = item.replace('"showWhenDataModelInvalid": "Hide"', '"showWhenDataModelInvalid": "Show"', 1)
        out += [tui[at:m.start()], item]
        at = end
        fixed += 1
    tui = "".join(out) + tui[at:]
    if fixed:
        sdk_build.log("  %d segment captions made visible" % fixed)
    # "steps": {"<parameter name>": [first, last]}: a whole-number parameter the Q-Links and the
    # data wheel must step through one at a time. An MPC moves a continuous parameter a hundredth
    # of its range per click; only one with "replacementStrings" in the skin gets exact steps,
    # and the SDK's tools write those for at most 64 choices. The strings are the signed numbers.
    steps = spec.get("steps") or {}
    if steps:
        index = {p["name"]: p["index"] for p in json.load(open(params_json))["params"]}
        entries = []
        for param, (first, last) in steps.items():
            if param not in index:
                raise sdk_build.BuildError("tabs.json steps: no parameter called \"%s\"" % param)
            if '"Parameter %d"' % index[param] in tui[tui.find('"replacementStrings"'):tui.find("]", tui.find('"replacementStrings"'))]:
                raise sdk_build.BuildError("tabs.json steps: \"%s\" already has steps" % param)
            strings = ", ".join('"%s%d"' % ("+" if v > 0 else "", v) for v in range(first, last + 1))
            entries.append('{"key": "Parameter %d", "value": [%s]}' % (index[param], strings))
        at = tui.find('"replacementStrings"')
        if at < 0:
            raise sdk_build.BuildError("tabs.json steps: the skin has no replacementStrings")
        at = tui.index("[", at) + 1
        empty = tui[at:tui.index("]", at)].strip() == ""
        tui = tui[:at] + "\n" + ",\n".join(entries) + ("\n" if empty else ",") + tui[at:]
        sdk_build.log("  tabs.json: %d parameters given exact steps" % len(steps))
    if not buttons and not moved:
        open(tui_path, "w", encoding="utf-8").write(tui)
        return
    # "place": {"<parameter name>": "x y w h"} moves any control, to make room beside it.
    for param, where in moved.items():
        at = tui.find('"name": "%s",' % param)
        if at < 0:
            raise sdk_build.BuildError("tabs.json place: no control for the parameter \"%s\"" % param)
        item_end = tui.index("}", tui.index('"boundsType"', at))
        item, n = re.subn(r'("bounds"\s*:\s*)"[^"]*"', lambda m: '%s"%s"' % (m.group(1), where), tui[at:item_end], count=1)
        if n != 1:
            raise sdk_build.BuildError("tabs.json place: \"%s\" has no bounds" % param)
        tui = tui[:at] + item + tui[item_end:]
    if not buttons:
        open(tui_path, "w", encoding="utf-8").write(tui)
        return
    key_at = tui.find('"key": "toggle"')
    if key_at < 0:
        raise sdk_build.BuildError("tabs.json buttons: the skin has no toggle to copy")
    start = tui.rfind("{", 0, key_at)
    template = tui[start:matching(tui, start)]
    new_blocks = []
    for image in sorted({b["image"] for b in buttons.values()}):
        sizes = {tuple(b["bounds"].split()[2:]) for b in buttons.values() if b["image"] == image}
        if len(sizes) != 1:
            raise sdk_build.BuildError("tabs.json buttons: \"%s\" is used at more than one size" % image)
        width, height = (int(v) for v in sizes.pop())
        for state in ("on", "off"):
            name = "%s_%s.png" % (image, state)
            source = os.path.join(project, "skin-images", name)
            if not os.path.isfile(source):
                raise sdk_build.BuildError("tabs.json buttons: no %s" % source)
            with open(source, "rb") as f, open(os.path.join(out_dir, "Plugin Skins", name), "wb") as g:
                g.write(f.read())
        block = template.replace('"key": "toggle"', '"key": "button %s"' % image, 1)
        # Its parts: the focus outline, the name, the value, the button. Keep the first and last.
        list_at = block.index("[", block.index('"componentsData"'))
        list_end = matching(block, list_at)
        parts, i = [], list_at + 1
        while True:
            i = block.find("{", i, list_end)
            if i < 0:
                break
            end = matching(block, i)
            parts.append(block[i:end])
            i = end
        if len(parts) != 4:
            raise sdk_build.BuildError("tabs.json buttons: the SDK's toggle has changed shape (%d parts)" % len(parts))
        bounds = re.compile(r'("bounds"\s*:\s*)"[^"]*"')
        focus = bounds.sub(lambda m: '%s"0 0 %d %d"' % (m.group(1), width, height), parts[0])
        button = bounds.sub(lambda m: '%s"6 6 %d %d"' % (m.group(1), width - 12, height - 12), parts[3])
        button = button.replace("toggle_on.png", image + "_on.png").replace("toggle_off.png", image + "_off.png")
        block = block[:list_at + 1] + "\n" + focus + ",\n" + button + "\n" + block[list_end - 1:]
        new_blocks.append(block)
    # "big": {"<parameter name>": "x y w h"}: the parameter's name-and-value bar at another size,
    # text included. The MPC draws the generic bar's text itself at one size, so this is built
    # from parts whose size the skin does set: the toggle's name and value labels with their
    # fonts scaled by h / 107 (the generator's row height), and a plain colour bar under them.
    big = spec.get("big") or {}
    for size in sorted({tuple(where.split()[2:]) for where in big.values()}):
        width, height = (int(v) for v in size)
        scale = height / 107.0
        slider_at = tui.find('"key": "sliderWithFocus"')
        if slider_at < 0:
            raise sdk_build.BuildError("tabs.json big: the skin has no slider to copy")
        slider_start = tui.rfind("{", 0, slider_at)
        block = tui[slider_start:matching(tui, slider_start)]
        block = block.replace('"key": "sliderWithFocus"', '"key": "big slider %dx%d"' % (width, height), 1)
        list_at = block.index("[", block.index('"componentsData"'))
        list_end = matching(block, list_at)
        first = block.index("{", list_at)
        focus_end = matching(block, first)
        bar_start = block.index("{", focus_end)
        bar = block[bar_start:matching(block, bar_start)]
        t_list = template.index("[", template.index('"componentsData"'))
        labels, i = [], t_list + 1
        for _ in range(3):
            i = template.index("{", i)
            end = matching(template, i)
            labels.append(template[i:end])
            i = end
        bounds = re.compile(r'("bounds"\s*:\s*)"[^"]*"')
        font = re.compile(r'("height"\s*:\s*)[0-9.]+')

        def sized(part, x, y, w, h, points=None):
            part = bounds.sub(lambda m: '%s"%d %d %d %d"' % (m.group(1), x, y, w, h), part)
            return font.sub(lambda m: "%s%.1f" % (m.group(1), points), part) if points else part

        inset = int(15 * scale)
        focus = sized(block[first:focus_end], 0, 0, width, height)
        name = sized(labels[1], inset, int(10 * scale), width - 2 * inset, int(32 * scale), 26 * scale)
        name = name.replace('"case": "Original"', '"case": "Upper Case"')
        value = sized(labels[2], inset, int(43 * scale), width - 2 * inset, int(40 * scale), 30 * scale)
        data_at = bar.index('"data"')
        data_start = bar.index("{", data_at)
        accent = re.search(r'"outlineColour"\s*:\s*"([0-9a-fA-F]{8})"', block).group(1)
        bar = (bar[:data_start]
               + '{"version": 2, "sliderType": "ColourBar", "direction": "Horizontal", "backgroundColour": "ff2c2b2c", '
                 '"outlineColour": "ff6a6a6c", "valueColour": "%s", "isBipolar": false, "handleName": "Data"}' % accent
               + bar[matching(bar, data_start):])
        bar = sized(bar, inset, int(88 * scale), width - 2 * inset, int(9 * scale))
        block = block[:list_at + 1] + "\n" + ",\n".join([focus, name, value, bar]) + "\n" + block[list_end - 1:]
        new_blocks.append(block)
    tui = tui[:start] + "".join(b + ",\n" for b in new_blocks) + tui[start:]
    for param, where in big.items():
        at = tui.find('"name": "%s",' % param)
        if at < 0:
            raise sdk_build.BuildError("tabs.json big: no control for the parameter \"%s\"" % param)
        item_end = tui.index("}", tui.index('"boundsType"', at))
        item, n = re.subn(r'("type"\s*:\s*)"sliderWithFocus"',
                          lambda m: '%s"big slider %s"' % (m.group(1), "x".join(where.split()[2:])), tui[at:item_end], count=1)
        item, m2 = re.subn(r'("bounds"\s*:\s*)"[^"]*"', lambda m: '%s"%s"' % (m.group(1), where), item, count=1)
        if n != 1 or m2 != 1:
            raise sdk_build.BuildError("tabs.json big: \"%s\" is not a plain slider on a page" % param)
        tui = tui[:at] + item + tui[item_end:]
    for param, b in buttons.items():
        at = tui.find('"name": "%s",' % param)
        if at < 0:
            raise sdk_build.BuildError("tabs.json buttons: no control for the parameter \"%s\"" % param)
        item_end = tui.index('"boundsType"', at)
        item_end = tui.index("}", item_end)
        item = tui[at:item_end]
        item, n = re.subn(r'("type"\s*:\s*)"toggle"', lambda m: '%s"button %s"' % (m.group(1), b["image"]), item, count=1)
        item, m2 = re.subn(r'("bounds"\s*:\s*)"[^"]*"', lambda m: '%s"%s"' % (m.group(1), b["bounds"]), item, count=1)
        if n != 1 or m2 != 1:
            raise sdk_build.BuildError("tabs.json buttons: \"%s\" is not a switch on a page" % param)
        tui = tui[:at] + item + tui[item_end:]
    open(tui_path, "w", encoding="utf-8").write(tui)
    sdk_build.log("  tabs.json: %d picture buttons, %d controls moved, %d made big" % (len(buttons), len(moved), len(big)))


_make_skins = sdk_build.make_skins


def make_skins(project, out_dir, params_json, no_ui):
    made = _make_skins(project, out_dir, params_json, no_ui)
    if made:
        apply_tabs(project, out_dir, params_json)
        apply_buttons(project, out_dir, params_json)
    return made


sdk_build.make_skins = make_skins

if __name__ == "__main__":
    sys.exit(sdk_build.main())
