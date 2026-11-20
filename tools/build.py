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
    # "ownText": parameters whose choices are not fixed: the folders, presets and sample files
    # found when the plugin starts. The SDK's tools read a parameter's choices from the build
    # machine's run of the plugin and write them into the skin ("replacementStrings"), and an MPC
    # then shows and steps through those and nothing else: on a Force with the full library the
    # Folder bar offered the three folders of the test set (2026-11-16). Their entries are taken
    # out, so the MPC shows the plugin's own text. (They then move as continuous parameters, a
    # hundredth of the range per click; the arrow buttons are the way to step.)
    own = spec.get("ownText") or []
    if own:
        index = {p["name"]: p["index"] for p in json.load(open(params_json))["params"]}
        removed = 0
        for param in own:
            if param not in index:
                raise sdk_build.BuildError("tabs.json ownText: no parameter called \"%s\"" % param)
            start_list = tui.find('"replacementStrings"')
            end_list = matching(tui, tui.index("[", start_list))
            m = re.search(r'\{\s*"key"\s*:\s*"Parameter %d"' % index[param], tui[start_list:end_list])
            if not m:
                continue
            a = start_list + m.start()
            b = matching(tui, a)
            # with the comma that separated it from a neighbour
            after = re.match(r"\s*,", tui[b:])
            if after:
                b += after.end()
            else:
                before = re.search(r",\s*$", tui[:a])
                if before:
                    a = before.start()
            tui = tui[:a] + tui[b:]
            removed += 1
        sdk_build.log("  tabs.json: %d parameters show the plugin's own text" % removed)
    # "steps": {"<parameter name>": [first, last]} or [first, last, {"<value>": "<word>"}]: a whole-number parameter the Q-Links and the
    # data wheel must step through one at a time. An MPC moves a continuous parameter a hundredth
    # of its range per click; only one with "replacementStrings" in the skin gets exact steps,
    # and the SDK's tools write those for at most 64 choices. The strings are the signed numbers.
    steps = spec.get("steps") or {}
    if steps:
        index = {p["name"]: p["index"] for p in json.load(open(params_json))["params"]}
        entries = []
        for param, limits in steps.items():
            first, last = limits[0], limits[1]
            named = limits[2] if len(limits) > 2 else {}  # {"-1": "Off"}: values the plugin shows as a word
            if param not in index:
                raise sdk_build.BuildError("tabs.json steps: no parameter called \"%s\"" % param)
            if '"Parameter %d"' % index[param] in tui[tui.find('"replacementStrings"'):tui.find("]", tui.find('"replacementStrings"'))]:
                raise sdk_build.BuildError("tabs.json steps: \"%s\" already has steps" % param)
            sign = "+" if first < 0 and not named else ""
            strings = ", ".join('"%s"' % named.get(str(v), "%s%d" % (sign if v > 0 else "", v)) for v in range(first, last + 1))
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


def apply_grid(project, out_dir, params_json):
    """tabs.json "grid": {"name": "GRID", "after": "GOLD"}: one more page, after the named one on
    its function key, that the SDK's generator cannot describe: the Deluge's 16 x 8 shortcut grid
    as a picture (skin-images/grid_bg.png, drawn by tools/make_grid.py, which also holds the
    geometry), and over it
      - in the strip at the top, the sixteen gold knobs as eight pairs, upper knob over lower:
        the knob's text and a bar, which the Q-Links control on this page as on the other;
      - in each cell that is a parameter, a bar showing its value, and over it a button that
        selects the cell; the selected cell's editor takes the strip's place (see the code).
    Seen on a Force 2026-11-18 in its first form, where a touch opened the kit's full-screen
    overlay: Jamie wanted the control on the page instead, hence the editor in the strip."""
    spec_path = os.path.join(project, "tabs.json")
    tui_path = os.path.join(out_dir, "Plugin Skins", "TUI.json")
    q_path = os.path.join(out_dir, "Plugin Skins", "Q-Links.json")
    if not (os.path.isfile(spec_path) and os.path.isfile(tui_path) and os.path.isfile(q_path)):
        return
    spec = json.load(open(spec_path))
    grid = spec.get("grid")
    if not grid:
        return
    import make_grid as g
    index = {p["name"]: p["index"] for p in json.load(open(params_json))["params"]}
    tui = open(tui_path, encoding="utf-8").read()

    key = sub = None
    for k, names in enumerate(spec["keys"]):
        if grid["after"] in names:
            key, sub = k, names.index(grid["after"])
            if len(names) >= 8:
                raise sdk_build.BuildError("tabs.json grid: no room on that function key")
    if key is None:
        raise sdk_build.BuildError("tabs.json grid: no page called \"%s\"" % grid["after"])
    # "qlinks": the page whose Q-Link assignments this one copies (the gold knobs' page).
    knobs_page = grid.get("qlinks", grid["after"])
    knobs_at = [(k, names.index(knobs_page)) for k, names in enumerate(spec["keys"]) if knobs_page in names]
    if not knobs_at:
        raise sdk_build.BuildError("tabs.json grid: no page called \"%s\"" % knobs_page)

    source = os.path.join(project, "skin-images", "grid_bg.png")
    if not os.path.isfile(source):
        raise sdk_build.BuildError("tabs.json grid: no %s (run tools/make_grid.py)" % source)
    with open(source, "rb") as f, open(os.path.join(out_dir, "Plugin Skins", "grid_bg.png"), "wb") as out:
        out.write(f.read())

    tabs_at = tui.index('"tabs"')

    def overlay_of(param):
        """The overlay the parameter's control on its own page opens, from that control's type."""
        at = tui.find('"value": "Parameter %d"' % index[param], tabs_at)
        if at < 0:
            raise sdk_build.BuildError("tabs.json grid: \"%s\" is on no page" % param)
        kind = re.findall(r'"type"\s*:\s*"([^"]*)"', tui[tui.rfind('"componentData"', 0, at):at])[0]
        definition = tui.find('"key": "%s"' % kind)
        if definition < 0:
            raise sdk_build.BuildError("tabs.json grid: \"%s\" is drawn by \"%s\", which is not a kit component" % (param, kind))
        start = tui.rfind("{", 0, definition)
        m = re.search(r'"handler"\s*:\s*"Show Overlay"[^}]*"additionalData"\s*:\s*"([^"]+)"', tui[start:matching(tui, start)])
        if not m:
            raise sdk_build.BuildError("tabs.json grid: \"%s\" opens no overlay" % kind)
        return m.group(1)

    accent = re.search(r'"outlineColour"\s*:\s*"([0-9a-fA-F]{8})"', tui).group(1)
    clear = {"version": 1, "focussed": {"version": 1, "colour": "00000000", "image": ""},
             "unfocussed": {"version": 1, "colour": "00000000", "image": ""}}

    def bounds(x, y, w, h, focus="No", show="Show", when="Always"):
        return {"version": 1, "acceptsHWFocus": focus, "showWhenDataModelInvalid": show, "whenVisible": when,
                "boundsType": "Absolute", "bounds": "%d %d %d %d" % (x, y, w, h)}

    def entry(name, kind, data, where, param=None):
        remap = [{"key": "Data", "value": "Parameter %d" % index[param]}] if param else []
        return {"version": 2, "componentData": {"version": 1, "name": name, "type": kind, "data": data},
                "handle remapping": {"version": 1, "map": remap}, "bounds": where}

    def focus_ring(w, h):
        return entry("focus", "Focus", {"version": 1, "backgroundColour": "19ffffff", "outlineColour": accent,
                                        "backgroundInset": 2.0, "outlineThickness": 1.0}, bounds(0, 0, w, h, when="WhenFocussed"))

    def bar(colour, bipolar):
        return {"version": 2, "sliderType": "ColourBar", "direction": "Horizontal", "backgroundColour": "ff141314",
                "outlineColour": "00000000", "valueColour": "ff%02x%02x%02x" % colour, "isBipolar": bipolar, "handleName": "Data"}

    def actions(*pairs):
        return [{"version": 1, "onAction": on, "handler": handler, "handleName": "Data", "additionalData": extra}
                for on, handler, extra in pairs]

    def component(acts, children):
        return {"version": 2, "actions": acts, "backgroundData": clear, "ignoreMousePresses": False,
                "disableCoarseDataWheel": False, "componentsData": children}

    named = g.cells()
    definitions = {}
    children = [entry("grid", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": "grid_bg.png"},
                      bounds(0, 0, g.WIDTH, g.HEIGHT))]
    # The gold knobs: pair p (0..7) is Gold 2p+1 over Gold 2p+2, as on the Deluge's panel.
    gold_w, gold_h = 160, (g.STRIP - 8) // 2
    text = {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": "SemiBold", "height": 17.0},
            "colour": "fff2f2f2", "justification": "horizontallyCentred verticallyCentred", "case": "Original"}
    definitions["grid gold"] = component(
        actions(("Mouse Down", "Q-Link", ""), ("Double Click", "Show Overlay", overlay_of("Gold 1")),
                ("Enter Pressed", "Show Overlay", overlay_of("Gold 1"))),
        [focus_ring(gold_w, gold_h),
         # The parameter's name is the preset's name for the knob on an MPC, and its text the
         # position alone (knobParameterName in DelugeSynth.cpp): the name, then the number.
         entry("name", "Label", {"version": 1, "textStyle": dict(text, justification="left verticallyCentred"), "type": "Name", "handleName": "Data"},
               bounds(6, 4, gold_w - 44, gold_h - 22)),
         entry("value", "Label", {"version": 1, "textStyle": dict(text, justification="right verticallyCentred"), "type": "Value", "handleName": "Data"},
               bounds(gold_w - 40, 4, 34, gold_h - 22)),
         entry("bar", "Slider", bar((0xe0, 0xb0, 0x40), False), bounds(8, gold_h - 15, gold_w - 16, 9))])
    for knob in range(16):
        param = "Gold %d" % (knob + 1)
        if param not in index:
            raise sdk_build.BuildError("tabs.json grid: no parameter called \"%s\"" % param)
        children.append(entry(param, "grid gold", {"version": 1, "handleName": "Data"},
                              bounds((knob // 2) * gold_w, 4 + (knob % 2) * gold_h, gold_w, gold_h, focus="Yes", show="Hide"), param))
    # The cells. Touching one selects it: every cell is a button of one radio group on a value
    # that lives in the page only ("Local"; 0 is no cell), lit as a ring round the cell. The
    # selected cell's editor (name, value, a wide bar to drag) then shows in the strip, over the
    # gold knobs, until its close button sets the value back to 0. A double tap on the editor's
    # text opens the full-screen overlay the parameter's own page uses (the list, for a choice).
    # In a cell the bar comes first and the button after, so the button is on top and takes
    # the touch that would otherwise drag the bar.
    for name in ("grid_sel_on.png", "grid_sel_off.png", "grid_close_on.png", "grid_close_off.png"):
        with open(os.path.join(project, "skin-images", name), "rb") as f, open(os.path.join(out_dir, "Plugin Skins", name), "wb") as out:
            out.write(f.read())
    ordered = sorted(named.items(), key=lambda item: (-item[0][1], item[0][0]))
    steps = len(ordered) + 1
    selected = "Local/%d/gridsel" % steps

    def radio(image, number):
        return {"version": 2, "onImage": image + "_on.png", "offImage": image + "_off.png", "buttonId": number,
                "numButtonsInGroup": steps, "handleName": selected, "gestureBehaviour": "Instant"}

    big = dict(text, font={"version": 1, "name": "Titillium Web", "style": "SemiBold", "height": 34.0},
               justification="left verticallyCentred", case="Upper Case")
    value = dict(big, font={"version": 1, "name": "Titillium Web", "style": "Regular", "height": 40.0}, colour="ff" + accent[2:], case="Original")
    editors = []
    for number, ((x, y), (param, minimum)) in enumerate(ordered, 1):
        if param not in index:
            raise sdk_build.BuildError("tabs.json grid: no parameter called \"%s\"" % param)
        x0, y0 = g.cell_origin(x, y)
        bx, by, bw, bh = g.BAR
        # Under the bar, the cell's look: standard, set, or not for this sound (make_grid.py).
        strip = "grid_cell_%d_%d.png" % (x, y)
        with open(os.path.join(project, "skin-images", strip), "rb") as f, open(os.path.join(out_dir, "Plugin Skins", strip), "wb") as out:
            out.write(f.read())
        if param + " *" not in index:
            raise sdk_build.BuildError("tabs.json grid: no parameter called \"%s *\"" % param)
        children.append(entry(param + " state", "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": strip,
                                                          "numFrames": 3, "handleName": "Data"},
                              bounds(x0, y0, g.CELL_W, g.CELL_H), param + " *"))
        # The cell proper is a component of its own, as the kit's controls are: it is what takes
        # the hardware focus when touched (a bare button did not: on a Force the focus stayed on
        # the MPC's plugin selector, and a push of the data wheel opened that, 2026-11-18), so
        # the data wheel then turns this parameter and a push opens its full-screen editor.
        overlay = overlay_of(param)
        definitions["grid cell %d" % number] = component(
            actions(("Enter Pressed", "Show Overlay", overlay)),
            [focus_ring(g.CELL_W, g.CELL_H),
             entry("bar", "Slider", bar((0xf2, 0xf2, 0xf2), minimum < 0), bounds(bx, by, bw, bh)),
             entry("select", "Button", radio("grid_sel", number), bounds(0, 0, g.CELL_W, g.CELL_H))])
        children.append(entry(param, "grid cell %d" % number, {"version": 1, "handleName": "Data"},
                              bounds(x0, y0, g.CELL_W, g.CELL_H, focus="Yes"), param))
        kind = "grid editor %s%s" % (overlay, " signed" if minimum < 0 else "")
        if kind not in definitions:
            definitions[kind] = component(
                actions(("Double Click", "Show Overlay", overlay), ("Enter Pressed", "Show Overlay", overlay)),
                [focus_ring(g.WIDTH, g.STRIP),
                 entry("name", "Label", {"version": 1, "textStyle": big, "type": "Name", "handleName": "Data"}, bounds(24, 10, 420, 44)),
                 entry("value", "Label", {"version": 1, "textStyle": value, "type": "Value", "handleName": "Data"}, bounds(24, 54, 420, 52)),
                 entry("bar", "Slider", bar((0xf2, 0xf2, 0xf2), minimum < 0), bounds(460, 26, 700, 64)),
                 entry("close", "Button", radio("grid_close", 0), bounds(1184, 18, 80, 80))])
            definitions[kind]["backgroundData"] = {"version": 1, "focussed": {"version": 1, "colour": "ff2c2b2c", "image": ""},
                                                   "unfocussed": {"version": 1, "colour": "ff2c2b2c", "image": ""}}
        where = bounds(0, 0, g.WIDTH, g.STRIP, focus="Yes")
        where["version"] = 2
        where["additionalInvalidatingHandles"] = ["IndexedEnabling/%d/%d/%s" % (number, steps, selected)]
        editors.append(entry(param, kind, {"version": 1, "handleName": "Data"}, where, param))
    children += editors
    page = component([], children)
    page["backgroundData"] = {"version": 1, "focussed": {"version": 1, "colour": "ff1e1d1e", "image": ""},
                              "unfocussed": {"version": 1, "colour": "ff1e1d1e", "image": ""}}
    tab = {"version": 1, "tabName": grid["name"], "fnKeyIndex": key, "fnKeySubIndex": sub + 1,
           "componentDefinition": page, "qlinkBoundsData": []}

    # "mini": {"page": "MAIN", "at": "x y"}: a small copy of the grid on another page, cells
    # without names, showing each pad's state (the same "<name> *" parameters) and its value:
    # the shape of the patch, redrawn as presets are stepped through. Not a control.
    small = grid.get("mini")
    if small:
        left, top = (int(v) for v in small["at"].split())
        parts = [entry("mini grid", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": "grid_mini_bg.png"},
                       bounds(left, top, 16 * g.MINI_W, 8 * g.MINI_H))]
        for (x, y), (param, _) in ordered:
            parts.append(entry(param + " mini", "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": "grid_mini_%d.png" % x,
                                                          "numFrames": 3, "handleName": "Data"},
                               bounds(left + x * g.MINI_W, top + (7 - y) * g.MINI_H, g.MINI_W, g.MINI_H), param + " *"))
            # The value, roughly: a thin bar along the cell's foot, hidden while the pad does not
            # apply to the sound (state 2 of 3).
            where = bounds(left + x * g.MINI_W + 7, top + (7 - y) * g.MINI_H + 21, g.MINI_W - 14, 6)
            where["version"] = 2
            where["additionalInvalidatingHandles"] = ["RangedEnabling/0/1/3/Parameter %d" % index[param + " *"]]
            thin = bar((0xf2, 0xf2, 0xf2), named[(x, y)][1] < 0)
            thin["backgroundColour"] = "59000000"
            parts.append(entry(param + " mini bar", "Slider", thin, where, param))
        # The bars are the parameters' own sliders, which a touch would drag. This is a picture,
        # not a control surface (that is the GRID page), so an empty component lies over it all
        # and takes the touches.
        definitions["grid mini cover"] = component([], [])
        parts.append(entry("mini cover", "grid mini cover", {"version": 1, "handleName": "Data"},
                           bounds(left, top, 16 * g.MINI_W, 8 * g.MINI_H)))
        for name in ["grid_mini_bg.png"] + ["grid_mini_%d.png" % x for x in range(16)]:
            with open(os.path.join(project, "skin-images", name), "rb") as f, open(os.path.join(out_dir, "Plugin Skins", name), "wb") as out:
                out.write(f.read())
        at = re.search(r'"tabName"\s*:\s*"%s"' % re.escape(small["page"]), tui)
        if not at:
            raise sdk_build.BuildError("tabs.json grid mini: no page called \"%s\"" % small["page"])
        tab_start = tui.rfind("{", 0, at.start())
        tab_end = matching(tui, tab_start)
        list_at = tui.index("[", tui.index('"componentsData"', tab_start, tab_end))
        list_end = matching(tui, list_at)
        tui = tui[:list_end - 1] + ",\n" + ",\n".join(json.dumps(part) for part in parts) + "\n" + tui[list_end - 1:]

    # The new components go before the first of the kit's; the tab after the page it follows.
    first = tui.rfind("{", 0, tui.index('"key": "sliderWithFocus"'))
    blocks = "".join(json.dumps({"key": name, "value": value}) + ",\n" for name, value in definitions.items())
    tui = tui[:first] + blocks + tui[first:]
    # Pages after it on the same key move along one.
    tui = re.sub(r'("tabName"\s*:\s*"[^"]*"\s*,\s*"fnKeyIndex"\s*:\s*%d\s*,\s*"fnKeySubIndex"\s*:\s*)(\d+)' % key,
                 lambda m: m.group(1) + str(int(m.group(2)) + (1 if int(m.group(2)) > sub else 0)), tui)
    after = re.search(r'"tabName"\s*:\s*"%s"' % re.escape(grid["after"]), tui)
    start = tui.rfind("{", 0, after.start())
    end = matching(tui, start)
    tui = tui[:end] + ",\n" + json.dumps(tab) + tui[end:]
    open(tui_path, "w", encoding="utf-8").write(tui)

    # Its Q-Links are a copy of the gold knobs' page's.
    q = open(q_path, encoding="utf-8").read()

    def q_entry(text, tab_number, sub_number):
        m = re.search(r'\{\s*"Tab"\s*:\s*%d\s*,\s*"SubTab"\s*:\s*%d\s*,' % (tab_number, sub_number), text)
        if not m:
            raise sdk_build.BuildError("tabs.json grid: Q-Links.json has no entry for tab %d.%d" % (tab_number, sub_number))
        return m.start(), matching(text, m.start())

    a0, a1 = q_entry(q, knobs_at[0][0] + 1, knobs_at[0][1] + 1)
    copy = re.sub(r'("Tab"\s*:\s*)\d+(\s*,\s*"SubTab"\s*:\s*)\d+', lambda m: "%s%d%s%d" % (m.group(1), key + 1, m.group(2), sub + 2),
                  q[a0:a1], count=1)
    q = re.sub(r'("Tab"\s*:\s*%d\s*,\s*"SubTab"\s*:\s*)(\d+)' % (key + 1),
               lambda m: m.group(1) + str(int(m.group(2)) + (1 if int(m.group(2)) > sub + 1 else 0)), q)
    _, end = q_entry(q, key + 1, sub + 1)
    q = q[:end] + ",\n" + copy + q[end:]
    open(q_path, "w", encoding="utf-8").write(q)
    sdk_build.log("  tabs.json: the %s page, %d cells and 16 gold knobs" % (grid["name"], len(named)))


def apply_about(project, out_dir, params_json):
    """tabs.json "about": {"name": "ABOUT", "after": "<the last page on a key>", "lines": [...]}: a
    page of fixed text, centred: the credits, drawn as one picture. A line is a string, or
    [string, font height] or [string, font height, "AARRGGBB"]; "" leaves a gap; {version}
    stands for plugin.json's version. Lines do not wrap: keep them under about 90 characters.
    "control": {"param": "<parameter name>", "bounds": "x y w h"} puts one of the kit's bars on the
    page for that parameter and gives it the page's first Q-Link."""
    spec_path = os.path.join(project, "tabs.json")
    tui_path = os.path.join(out_dir, "Plugin Skins", "TUI.json")
    if not (os.path.isfile(spec_path) and os.path.isfile(tui_path)):
        return
    spec = json.load(open(spec_path))
    about = spec.get("about")
    if not about:
        return
    place = [(k, names.index(about["after"])) for k, names in enumerate(spec["keys"]) if about["after"] in names]
    if not place or place[0][1] != len(spec["keys"][place[0][0]]) - 1:
        raise sdk_build.BuildError("tabs.json about: \"%s\" must be the last page on a function key" % about.get("after"))
    key, sub = place[0]
    tui = open(tui_path, encoding="utf-8").read()
    # The text is a picture (tools/make_about.py says why), redrawn here when Pillow is there.
    try:
        import make_about
        make_about.render(project)
    except ImportError:
        pass
    source = os.path.join(project, "skin-images", "about.png")
    if not os.path.isfile(source):
        raise sdk_build.BuildError("tabs.json about: no %s (run tools/make_about.py)" % source)
    with open(source, "rb") as f, open(os.path.join(out_dir, "Plugin Skins", "about.png"), "wb") as out:
        out.write(f.read())
    children = [{"version": 2,
                 "componentData": {"version": 1, "name": "credits", "type": "Image",
                                   "data": {"version": 2, "imageType": "Regular", "colour": "0", "image": "about.png"}},
                 "handle remapping": {"version": 1, "map": []},
                 "bounds": {"version": 1, "acceptsHWFocus": "No", "showWhenDataModelInvalid": "Show", "whenVisible": "Always",
                            "boundsType": "Absolute", "bounds": "0 0 1280 628"}}]
    control = about.get("control")
    control_index = -1
    if control:
        index = {p["name"]: p["index"] for p in json.load(open(params_json))["params"]}
        if control["param"] not in index:
            raise sdk_build.BuildError("tabs.json about: no parameter called \"%s\"" % control["param"])
        control_index = index[control["param"]]
        children.append({"version": 2,
                         "componentData": {"version": 1, "name": control["param"], "type": "sliderWithFocus",
                                           "data": {"version": 1, "handleName": "Data"}},
                         "handle remapping": {"version": 1, "map": [{"key": "Data", "value": "Parameter %d" % control_index}]},
                         "bounds": {"version": 1, "acceptsHWFocus": "Yes", "showWhenDataModelInvalid": "Hide", "whenVisible": "Always",
                                    "boundsType": "Absolute", "bounds": control["bounds"]}})
    background = {"version": 1, "focussed": {"version": 1, "colour": "ff1e1d1e", "image": ""},
                  "unfocussed": {"version": 1, "colour": "ff1e1d1e", "image": ""}}
    tab = {"version": 1, "tabName": about["name"], "fnKeyIndex": key, "fnKeySubIndex": sub + 1,
           "componentDefinition": {"version": 2, "actions": [], "backgroundData": background, "ignoreMousePresses": False,
                                   "disableCoarseDataWheel": False, "componentsData": children},
           "qlinkBoundsData": []}
    after = re.search(r'"tabName"\s*:\s*"%s"' % re.escape(about["after"]), tui)
    start = tui.rfind("{", 0, after.start())
    end = matching(tui, start)
    open(tui_path, "w", encoding="utf-8").write(tui[:end] + ",\n" + json.dumps(tab) + tui[end:])
    # An entry in Q-Links.json like every other page's, with no knob assigned.
    q_path = os.path.join(out_dir, "Plugin Skins", "Q-Links.json")
    q = open(q_path, encoding="utf-8").read()
    m = re.search(r'\{\s*"Tab"\s*:\s*%d\s*,\s*"SubTab"\s*:\s*%d\s*,' % (key + 1, sub + 1), q)
    if not m:
        raise sdk_build.BuildError("tabs.json about: Q-Links.json has no entry for \"%s\"" % about["after"])
    end = matching(q, m.start())
    copy = re.sub(r'("SubTab"\s*:\s*)\d+', lambda n: n.group(1) + str(sub + 2), q[m.start():end], count=1)
    copy = re.sub(r'("Q-Link \d+"\s*:\s*)-?\d+', lambda n: n.group(1) + "-1", copy)
    if control_index >= 0:
        copy = re.sub(r'("Q-Link 1"\s*:\s*)-?\d+', lambda n: n.group(1) + str(control_index), copy, count=1)
    open(q_path, "w", encoding="utf-8").write(q[:end] + ",\n" + copy + q[end:])
    sdk_build.log("  tabs.json: the %s page, %d lines" % (about["name"], len([l for l in about["lines"] if l])))


_make_skins = sdk_build.make_skins


def make_skins(project, out_dir, params_json, no_ui):
    made = _make_skins(project, out_dir, params_json, no_ui)
    if made:
        apply_tabs(project, out_dir, params_json)
        apply_buttons(project, out_dir, params_json)
        apply_grid(project, out_dir, params_json)
        apply_about(project, out_dir, params_json)
    return made


sdk_build.make_skins = make_skins

if __name__ == "__main__":
    sys.exit(sdk_build.main())
