#!/usr/bin/env python3
"""Draw the background of Deluge Synth's GRID page: plugins/deluge-synth/skin-images/grid_bg.png,
checked in. The Deluge's 16 x 8 shortcut grid with each pad's name on it, under a strip left
plain for the gold knobs. tools/build.py ("grid" in tabs.json) puts the live parts over it: a bar
in each cell and the touch that opens the cell's parameter.

    python3 tools/make_grid.py        (needs Pillow; only to change how it looks)

The geometry below is shared with tools/build.py, which imports this file: change it here only.
"""
import os
import re

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, "plugins", "deluge-synth", "skin-images", "grid_bg.png")

WIDTH, HEIGHT = 1280, 628     # a tab's area on every MPC
STRIP = 116                   # the gold knobs' strip above the grid
CELL_W, CELL_H = 80, 64       # 16 columns, 8 rows
BAR = (6, 50, 68, 8)          # a cell's value bar, within the cell: x y w h
BACKGROUND = (0x1e, 0x1d, 0x1e)

# One colour per column: the Deluge's modules, a pair of columns each.
COLUMN_COLOURS = [
    (0x4c, 0xc2, 0x6b), (0x4c, 0xc2, 0x6b),   # sample 1, 2
    (0x4a, 0x9d, 0xf0), (0x4a, 0x9d, 0xf0),   # oscillator 1, 2
    (0xa9, 0x7b, 0xf5), (0xa9, 0x7b, 0xf5),   # FM modulator 1, 2
    (0xf0, 0xb4, 0x3c), (0xf0, 0xb4, 0x3c),   # master, voice
    (0xf0, 0x6a, 0x5a), (0xf0, 0x8a, 0x4a),   # envelope 1 + LPF, envelope 2 + HPF
    (0x3c, 0xc8, 0xb4), (0x3c, 0xc8, 0xb4),   # sidechain + bass, arpeggiator + treble
    (0xf0, 0x6e, 0xb4), (0xf0, 0x6e, 0xb4),   # LFO 1 + mod FX, LFO 2 + reverb
    (0x5a, 0xc8, 0xf0), (0x9a, 0x9a, 0xa0),   # delay, randomizer
]
GREY = (0x8a, 0x8a, 0x90)

# Pads that are not parameters, named so the grid reads like the panel. Mod sources are the
# firmware's modSourceShortcuts (gui/ui/sound_editor.cpp); the rest are gen_grid.py's SKIPPED.
PLAIN = {
    (14, 5): "LFO 1", (14, 6): "Env 1", (14, 7): "X",
    (15, 0): "After touch", (15, 4): "Side chain", (15, 5): "LFO 2", (15, 6): "Env 2", (15, 7): "Y",
    (0, 4): "S1 Record", (1, 4): "S2 Record", (0, 5): "S1 Browse", (1, 5): "S2 Browse",
    (0, 7): "S1 Start", (1, 7): "S2 Start", (4, 7): "Play Dir", (10, 5): "SC Send", (11, 5): "Name",
}


def cell_origin(x, y):
    """Top-left of pad (column x, row y from the bottom) on the page."""
    return x * CELL_W, STRIP + (7 - y) * CELL_H


def cells():
    """{(x, y): (name, minimum)} for the first-press pads that are parameters, from gen_grid.py's
    names and the checked-in table's ranges."""
    import gen_grid
    table = open(gen_grid.OUT).read()
    minimum = {}
    for m in re.finditer(r"\{(\d), +(\d+), (\d), Kind::\w+, +(-?\d+),", table):
        minimum[(int(m.group(1)), int(m.group(2)), int(m.group(3)))] = int(m.group(4))
    return {(x, y): (name, minimum[(layer, x, y)]) for (layer, x, y), name in gen_grid.NAMES.items() if layer == 0}


def main():
    from PIL import Image, ImageDraw, ImageFont

    def font(size):
        for path in ("/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf",
                     "/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf"):
            if os.path.exists(path):
                return ImageFont.truetype(path, size)
        return ImageFont.load_default(size * 0.7)

    scale = 3   # drawn large and reduced, for smooth edges
    image = Image.new("RGB", (WIDTH * scale, HEIGHT * scale), BACKGROUND)
    draw = ImageDraw.Draw(image)

    def mix(colour, amount):
        return tuple(int(b + (c - b) * amount) for c, b in zip(colour, BACKGROUND))

    def label(x0, y0, text, colour, height):
        """Up to two lines, centred in the cell's top `height` px, as large as fits."""
        words = text.upper().split()
        lines = [" ".join(words)] if len(words) == 1 else [words[0], " ".join(words[1:])]
        size = 19
        while size > 9:
            f = font(size * scale)
            if max(draw.textlength(line, font=f) for line in lines) <= (CELL_W - 10) * scale:
                break
            size -= 1
        line_h = (size + 1) * scale
        top = y0 * scale + (height * scale - line_h * len(lines)) / 2
        for i, line in enumerate(lines):
            w = draw.textlength(line, font=f)
            draw.text((x0 * scale + (CELL_W * scale - w) / 2, top + i * line_h), line, font=f, fill=colour)

    def box(x0, y0, fill, outline):
        draw.rounded_rectangle([(x0 + 2) * scale, (y0 + 2) * scale, (x0 + CELL_W - 2) * scale - 1, (y0 + CELL_H - 2) * scale - 1],
                               radius=6 * scale, fill=fill, outline=outline, width=scale)

    named = cells()
    for x in range(16):
        for y in range(8):
            x0, y0 = cell_origin(x, y)
            if (x, y) in named:
                colour = COLUMN_COLOURS[x]
                box(x0, y0, mix(colour, 0.08), mix(colour, 0.32))
                label(x0, y0 + 3, named[(x, y)][0], mix((0xf2, 0xf2, 0xf2), 0.6), BAR[1] - 5)
            elif (x, y) in PLAIN:
                box(x0, y0, BACKGROUND, mix(GREY, 0.35))
                label(x0, y0, PLAIN[(x, y)], mix(GREY, 0.8), CELL_H)
    # Each parameter cell's three looks as a film strip, top to bottom: standard (nothing: the
    # background shows), set to something else (the column's colour, solid), does not apply to
    # the sound (dark, the name barely there). The plugin's "<name> *" parameter picks the frame.
    folder = os.path.dirname(OUT)
    for (x, y), (name, _) in named.items():
        colour = COLUMN_COLOURS[x]
        strip = Image.new("RGBA", (CELL_W * scale, 3 * CELL_H * scale), (0, 0, 0, 0))
        pen = ImageDraw.Draw(strip)
        for frame, fill, outline, ink in ((1, mix(colour, 0.62), colour, (255, 255, 255)),
                                          (2, BACKGROUND, mix(GREY, 0.12), mix(GREY, 0.3))):
            top = frame * CELL_H
            pen.rounded_rectangle([2 * scale, (top + 2) * scale, (CELL_W - 2) * scale - 1, (top + CELL_H - 2) * scale - 1],
                                  radius=6 * scale, fill=fill + (255,), outline=outline + (255,), width=scale)
            saved = draw
            draw = pen
            label(0, top + 3, name, ink + (255,), BAR[1] - 5)
            draw = saved
        strip.resize((CELL_W, 3 * CELL_H), Image.LANCZOS).save(os.path.join(folder, "grid_cell_%d_%d.png" % (x, y)), optimize=True)
    # The gold strip: a faint line between the eight pairs of knobs.
    for pair in range(1, 8):
        draw.line([pair * 160 * scale, 8 * scale, pair * 160 * scale, (STRIP - 8) * scale], fill=mix(GREY, 0.25), width=scale)
    image.resize((WIDTH, HEIGHT), Image.LANCZOS).save(OUT, optimize=True)
    # The ring that marks the selected cell (and nothing, for the others), and the button that
    # closes the selected cell's editor.
    Image.new("RGBA", (CELL_W, CELL_H), (0, 0, 0, 0)).save(os.path.join(folder, "grid_sel_off.png"))
    ring = Image.new("RGBA", (CELL_W * scale, CELL_H * scale), (0, 0, 0, 0))
    ImageDraw.Draw(ring).rounded_rectangle([scale, scale, (CELL_W - 1) * scale - 1, (CELL_H - 1) * scale - 1], radius=7 * scale,
                                           outline=(255, 255, 255, 255), width=3 * scale)
    ring.resize((CELL_W, CELL_H), Image.LANCZOS).save(os.path.join(folder, "grid_sel_on.png"))
    for state, fill in (("off", (0x45, 0x44, 0x46, 255)), ("on", (0x45, 0x44, 0x46, 255))):
        close = Image.new("RGBA", (80 * scale, 80 * scale), (0, 0, 0, 0))
        d = ImageDraw.Draw(close)
        d.rounded_rectangle([0, 0, 80 * scale - 1, 80 * scale - 1], radius=12 * scale, fill=fill)
        for a0, b0, a1, b1 in ((26, 26, 54, 54), (26, 54, 54, 26)):
            d.line([a0 * scale, b0 * scale, a1 * scale, b1 * scale], fill=(255, 255, 255, 255), width=5 * scale)
        close.resize((80, 80), Image.LANCZOS).save(os.path.join(folder, "grid_close_%s.png" % state))
    print("wrote", os.path.relpath(OUT, HERE), "(%d cells with a parameter)" % len(named))


if __name__ == "__main__":
    main()
