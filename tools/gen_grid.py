#!/usr/bin/env python3
"""Write plugins/deluge-synth/src/grid_table.h: the pads of the Deluge's shortcut grid that Deluge
Synth offers as parameters, with the range, options and value text the firmware gives each.

A plugin declares its parameters before any preset is loaded (and the Intel build has no firmware
in it at all), so what the firmware would say is collected here, once, and checked in. The source
is tests/matrix_table.cpp run over every SYNTH preset on a Deluge card: a pad is described by the
presets it applies to, and every such preset must describe it the same way, or this stops.

    tools/gen_grid.py [card root]        default: build/corpus/root (made by tools/corpus.sh)
    tools/gen_grid.py --check [root]     compare with the checked-in header instead of writing it

Needs the native firmware build, so a 64-bit ARM computer. Run it again after moving the firmware
pin (tools/pins.sh). THE ORDER OF THE TABLE IS THE ORDER OF THE PLUGIN'S PARAMETERS, which MPC
projects depend on: a pad that appears later goes in APPENDED below, never into the middle.
"""
import collections
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, "plugins", "deluge-synth", "src", "grid_table.h")

# (layer, x, y) -> parameter name on the MPC. x is the grid column, y the row from the bottom, as
# in the firmware's paramShortcutsForSounds (gui/ui/menus.cpp). Short, because the MPC shows the
# name above each control, and unique, because its automation list shows nothing else.
# A pad of the grid that is not listed is not offered; see SKIPPED.
NAMES = collections.OrderedDict()


def column(layer, x, names):
    for y, name in names.items():
        NAMES[(layer, x, y)] = name


column(0, 0, {0: "S1 Repeat", 1: "S1 Reverse", 2: "S1 Speed", 3: "S1 Pitch/Spd", 6: "S1 Interp"})
column(0, 1, {0: "S2 Repeat", 1: "S2 Reverse", 2: "S2 Speed", 3: "S2 Pitch/Spd", 6: "S2 Interp"})
column(0, 2, {0: "Osc1 Level", 1: "Osc1 Transp", 2: "Osc1 Type", 3: "Osc1 PW", 4: "Osc1 Phase", 5: "Osc1 Feedbk",
              6: "Osc1 WaveIdx", 7: "Noise"})
column(0, 3, {0: "Osc2 Level", 1: "Osc2 Transp", 2: "Osc2 Type", 3: "Osc2 PW", 4: "Osc2 Phase", 5: "Osc2 Feedbk",
              6: "Osc2 WaveIdx", 7: "Osc Sync"})
column(0, 4, {0: "Mod1 Level", 1: "Mod1 Transp", 4: "Mod1 Phase", 5: "Mod1 Feedbk"})
column(0, 5, {0: "Mod2 Level", 1: "Mod2 Transp", 4: "Mod2 Phase", 5: "Mod2 Feedbk", 6: "Mod2 Dest", 7: "Stutter Rate"})
column(0, 6, {0: "Master Level", 1: "Transpose", 2: "Vibrato", 3: "Pan", 4: "Synth Mode", 5: "Decimation",
              6: "Bitcrush", 7: "Saturation"})
column(0, 7, {0: "Portamento", 1: "Polyphony", 2: "Priority", 3: "Uni Detune", 4: "Uni Number", 5: "Comp Thresh",
              7: "Wavefold"})
column(0, 8, {0: "Env1 Release", 1: "Env1 Sustain", 2: "Env1 Decay", 3: "Env1 Attack", 4: "LPF Morph", 5: "LPF Mode",
              6: "LPF Reso", 7: "LPF Freq"})
column(0, 9, {0: "Env2 Release", 1: "Env2 Sustain", 2: "Env2 Decay", 3: "Env2 Attack", 4: "HPF Morph", 5: "HPF Mode",
              6: "HPF Reso", 7: "HPF Freq"})
column(0, 10, {0: "SC Release", 1: "SC Sync", 2: "SC Ducking", 3: "SC Attack", 4: "SC Shape", 6: "Bass", 7: "Bass Freq"})
column(0, 11, {0: "Arp Rate", 1: "Arp Sync", 2: "Arp Gate", 3: "Arp Octaves", 4: "Arp Mode", 6: "Treble",
               7: "Treble Freq"})
column(0, 12, {0: "LFO1 Rate", 1: "LFO1 Sync", 2: "LFO1 Shape", 3: "ModFX Type", 4: "ModFX Offset", 5: "ModFX Feedbk",
               6: "ModFX Depth", 7: "ModFX Rate"})
column(0, 13, {0: "LFO2 Rate", 1: "LFO2 Sync", 2: "LFO2 Shape", 3: "Rev Amount", 4: "Rev Pan", 5: "Rev Diffusion",
               6: "Rev Damping", 7: "Rev Time"})
column(0, 14, {0: "Delay Rate", 1: "Delay Sync", 2: "Delay Type", 3: "Delay Amount", 4: "Delay PingPong"})
column(0, 15, {1: "Vel Spread", 2: "Rand Lock", 3: "Note Prob"})
# The second layer: what a second press of the same pad reaches.
column(1, 5, {7: "Stutter Dir"})
column(1, 7, {1: "Max Voices", 4: "Uni Spread", 5: "Comp Ratio"})
column(1, 8, {0: "Env3 Release", 1: "Env3 Sustain", 2: "Env3 Decay", 3: "Env3 Attack"})
column(1, 9, {0: "Env4 Release", 1: "Env4 Sustain", 2: "Env4 Decay", 3: "Env4 Attack"})
column(1, 12, {0: "LFO3 Rate", 1: "LFO3 Sync", 2: "LFO3 Shape"})
column(1, 13, {0: "LFO4 Rate", 1: "LFO4 Sync", 2: "LFO4 Shape"})
# APPENDED: pads added after 2026-11-14 go here, in the order they were added.

# Pads the firmware has that are deliberately not parameters, and why. Anything else the firmware
# reports that is in neither list stops the run: decide where it goes.
SKIPPED = {
    (0, 0, 4): "record to sample 1: a recorder, not a value",
    (0, 1, 4): "record to sample 2",
    (0, 0, 5): "sample 1 file browser",
    (0, 1, 5): "sample 2 file browser",
    (0, 0, 7): "sample 1 start point: the waveform editor",
    (0, 1, 7): "sample 2 start point",
    (0, 4, 7): "play direction: belongs to the Deluge's clip, not to the sound",
    (0, 10, 5): "send to sidechain: kits only",
    (0, 11, 5): "edit name",
}

KINDS = {"number": "Kind::Number", "choice": "Kind::Choice", "toggle": "Kind::Toggle"}


def standards(exe, held):
    """Each pad's standard value, which the GRID page marks departures from: what an empty sound
    holds (plugins/deluge-synth/starter/INIT.XML, in subtractive mode and then in FM mode), and
    for a pad that applies to neither (the sample pads: an empty sound has no sample) the value
    most presets on the card hold."""
    import shutil
    root = os.path.join(HERE, "build", "gen_grid_init")
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, "SYNTHS"))
    shutil.copy(os.path.join(HERE, "plugins", "deluge-synth", "starter", "INIT.XML"), os.path.join(root, "SYNTHS", "INIT.XML"))
    text = subprocess.run([exe, root, "SYNTHS"], check=True, stdout=subprocess.PIPE,
                          env=dict(os.environ, MATRIX_FM="1")).stdout.decode("utf-8", "replace")
    out = {}
    for tag in ("VALUE", "FMVALUE"):
        for line in text.splitlines():
            f = line.split("\t")
            if f[0] == tag:
                out.setdefault((int(f[1]), int(f[2]), int(f[3])), int(f[4]))
    for key, counts in held.items():
        if key not in out:
            out[key] = counts.most_common(1)[0][0]
    return out


def collect(root):
    exe = os.path.join(HERE, "build", "firmware", "native", "matrix_table")
    subprocess.run([sys.executable, os.path.join(HERE, "tools", "build_firmware.py"), "native", "--test",
                    os.path.join(HERE, "tests", "matrix_table.cpp")], check=True, stdout=subprocess.DEVNULL)
    text = subprocess.run([exe, root, "SYNTHS"], check=True, stdout=subprocess.PIPE).stdout.decode("utf-8", "replace")
    forms = collections.defaultdict(collections.Counter)
    held = collections.defaultdict(collections.Counter)
    skipped = {}
    presets = 0
    for line in text.splitlines():
        f = line.split("\t")
        if f[0] == "PRESET":
            presets += 1
        elif f[0] == "CELL":
            forms[(int(f[1]), int(f[2]), int(f[3]))]["\t".join(f[4:])] += 1
        elif f[0] == "VALUE":
            held[(int(f[1]), int(f[2]), int(f[3]))][int(f[4])] += 1
        elif f[0] == "SKIP":
            skipped[(int(f[1]), int(f[2]), int(f[3]))] = f[7]
    return presets, forms, skipped, standards(exe, held)


def describe(key, forms):
    """One description of a pad from the forms its presets gave. Two differences between presets
    are expected and settled here; any other is an error."""
    parsed = []
    for form, count in forms.items():
        f = form.split("\t")
        texts = collections.OrderedDict()
        for item in f[6:]:
            value, text = item.split("=", 1)
            # A multisample preset adds the key zone to an oscillator's transpose: "-12.00 (Bottom-C4)".
            if " (" in text and text.endswith(")") and f[0] == "number":
                text = text[:text.index(" (")]
            texts[int(value)] = text
        parsed.append((f[0], int(f[1]), int(f[2]), f[3], f[5], texts, count))
    # In ring-mod mode an oscillator's type list stops before Sample, DX7 and Input: keep the full
    # list (the firmware clamps a value past the end).
    parsed.sort(key=lambda p: -p[2])
    kind, lo, hi, cls, title, texts, _ = parsed[0]
    for other in parsed[1:]:
        if other[0] != kind or other[1] != lo or other[3] != cls:
            sys.exit("pad %s: presets disagree on kind, minimum or class: %s / %s" % (key, parsed[0][:4], other[:4]))
        if other[2] != hi and kind != "choice":
            sys.exit("pad %s: presets disagree on the maximum: %d / %d" % (key, hi, other[2]))
        for value, text in other[5].items():
            if texts.get(value, text) != text:
                sys.exit("pad %s: value %d reads '%s' in some presets and '%s' in others" % (key, value, texts[value], text))
    return kind, lo, hi, cls, title, texts


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def generate(root):
    presets, forms, skipped, standard = collect(root)
    if presets == 0:
        sys.exit("no presets under %s/SYNTHS" % root)
    for key in sorted(set(forms) | set(skipped)):
        if key not in NAMES and key not in SKIPPED:
            sys.exit("pad %s (%s) is in the firmware but in neither NAMES nor SKIPPED" % (key, skipped.get(key, "")))
    out = []
    tables = []
    cells = []
    for index, (key, name) in enumerate(NAMES.items()):
        if key not in forms:
            sys.exit("pad %s (%s): no preset under %s uses it, so its range is unknown" % (key, name, root))
        kind, lo, hi, cls, title, texts = describe(key, forms[key])
        full = hi - lo <= 400 and len(texts) == hi - lo + 1
        plain = [v for v, t in texts.items() if t != str(v)]
        hundredths = all(t == "%.2f" % (v / 100.0) for v, t in texts.items())
        table = exceptions = "nullptr"
        count = 0
        if kind != "number":
            if not full:
                sys.exit("pad %s: a %s with %d values" % (key, kind, hi - lo + 1))
            rule = "Table"
        elif hundredths and plain:
            rule = "Hundredths"
        elif not full:
            sys.exit("pad %s (%s): range %d..%d is too wide to list and is not hundredths" % (key, name, lo, hi))
        elif len(plain) <= 4:
            rule = "Plain"
            if plain:
                exceptions = "kExceptions%d" % index
                count = len(plain)
                tables.append("inline constexpr Exception %s[] = {%s};" % (
                    exceptions, ", ".join("{%d, %s}" % (v, c_string(texts[v])) for v in plain)))
        else:
            rule = "Table"
        if rule == "Table":
            table = "kTexts%d" % index
            tables.append("inline constexpr const char* %s[] = {%s};" % (
                table, ", ".join(c_string(texts[v]) for v in range(lo, hi + 1))))
        default = min(max(0, lo), hi)
        cells.append('    {%d, %2d, %d, %-13s %5d, %5d, %2d, %5d, "g%d_%d_%d", %-17s Text::%-11s %s, %s, %d}, // %s (%s)' % (
            key[0], key[1], key[2], KINDS[kind] + ",", lo, hi, default, min(max(standard[key], lo), hi), key[0], key[1], key[2], c_string(name) + ",",
            rule + ",", table, exceptions, count, title or name, cls))
    out.append("""// GENERATED by tools/gen_grid.py from the firmware's own menu items; do not edit by hand.
//
// The pads of the Deluge's shortcut grid that Downrush offers as parameters: where each is on
// the grid, its range, and the text the firmware shows for its values. The order is the order of
// the plugin's parameters, which MPC projects depend on: new pads are appended (see the tool).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace grid {

enum class Kind : uint8_t { Number, Choice, Toggle };
// How a value is shown: as the number itself; as the number over 100 with two decimals (pitch in
// semitones and cents, modulation depth); or from a list holding every value's text.
enum class Text : uint8_t { Plain, Hundredths, Table };
struct Exception { int16_t value; const char* text; }; // a Plain value that reads differently ("Off")

struct Cell {
    uint8_t layer, x, y;  // layer 0: first press; 1: second press. x: column; y: row from the bottom
    Kind kind;
    int16_t min, max, initial;
    int16_t standard;     // what an empty sound holds: the GRID page marks a pad that differs
    const char* id;       // the parameter's permanent id
    const char* name;
    Text text;
    const char* const* table;     // Text::Table: the text of min, min + 1, ... max
    const Exception* exceptions;
    uint8_t numExceptions;
};
""")
    out.append("\n".join(tables))
    out.append("\ninline constexpr Cell kCells[] = {")
    out.append("\n".join(cells))
    out.append("};\ninline constexpr int kNumCells = (int)(sizeof kCells / sizeof kCells[0]);\n\n} // namespace grid\n")
    return "\n".join(out), presets, len(cells)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    root = args[0] if args else os.path.join(HERE, "build", "corpus", "root")
    text, presets, cells = generate(root)
    if "--check" in sys.argv:
        if not os.path.exists(OUT) or open(OUT).read() != text:
            sys.exit("%s is not what the firmware says now: run tools/gen_grid.py and look at the difference" % os.path.relpath(OUT, HERE))
        print("grid table: %d pads, as the firmware describes them over %d presets" % (cells, presets))
        return
    with open(OUT, "w") as f:
        f.write(text)
    print("wrote %s: %d pads from %d presets" % (os.path.relpath(OUT, HERE), cells, presets))


if __name__ == "__main__":
    main()
