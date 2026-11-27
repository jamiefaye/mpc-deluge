#!/usr/bin/env python3
"""Draw what tabs.json "panels" asks for: for each page named there a background picture
(skin-images/panel_<PAGE>.png) of tinted panels with a heading each, and for each colour used a
copy of the kit's two knob film strips with the kit's accent turned to that colour
(skin-images/knob_uni_<rrggbb>.png, knob_bi_<rrggbb>.png). All checked in.

The pages the SDK's generator makes are sixteen grey-and-red controls on a blank background
(Jamie, 2026-11-25: "the rest looks plain"). A panel groups the controls of one module and takes
the colour its column has on the GRID page (COLUMN_COLOURS in make_grid.py). The headings are in
the picture because a skin's fixed-text labels do not show on a Force (see make_about.py).

tools/build.py runs this at every build when Pillow is there (apply_panels, which also holds what
is done to the skin), and gives the folder the generator put the kit's strips in.

    python3 tools/make_panels.py [plugin folder] [folder holding knob_uni.png and knob_bi.png]
"""
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIDTH, HEIGHT = 1280, 628
BACKGROUND = (0x1e, 0x1d, 0x1e)
# The generator's grid of controls: four columns, four rows, each control 278 x 107.
COLUMN_LEFT, COLUMN_STEP = 23, 320
ROW_TOPS = (45, 196, 346, 497)
CELL_W, CELL_H = 278, 107
HEADING = 30
KIT_ACCENT = (0xd3, 0x11, 0x45)  # mpc-red, the kit the strips are made in


def rgb(colour):
    return tuple(int(colour[i:i + 2], 16) for i in (0, 2, 4))


def cell_of(bounds):
    """(column, row) of a control from its bounds on the page, or None off the grid."""
    x, y = (int(v) for v in bounds.split()[:2])
    column = (x - COLUMN_LEFT) // COLUMN_STEP
    rows = [r for r, top in enumerate(ROW_TOPS) if abs(y - top) < 40]
    return (column, rows[0]) if rows and 0 <= column < 4 else None


def box(cells):
    """A panel's rectangle on the page from "column row columns rows"."""
    column, row, columns, rows = (int(v) for v in cells.split())
    return (column * COLUMN_STEP + 10, ROW_TOPS[row] - 37,
            (column + columns) * COLUMN_STEP - 10, ROW_TOPS[row + rows - 1] + CELL_H + 16)


def holds(cells, cell):
    column, row, columns, rows = (int(v) for v in cells.split())
    return column <= cell[0] < column + columns and row <= cell[1] < row + rows


def mix(colour, amount, under=BACKGROUND):
    return tuple(round(under[i] * (1 - amount) + colour[i] * amount) for i in range(3))


def tinted(strip, colour):
    """The strip with the kit's accent turned to the colour: each pixel is taken as so much
    accent over grey, the amount read from how far red stands above green and blue."""
    from PIL import Image, ImageChops
    r, g, b, a = strip.convert("RGBA").split()
    span = KIT_ACCENT[0] - max(KIT_ACCENT[1], KIT_ACCENT[2])
    amount = ImageChops.subtract(r, ImageChops.lighter(g, b)).point(lambda v: min(255, v * 255 // span))
    out = []
    for channel, was, to in zip((r, g, b), KIT_ACCENT, colour):
        change = to - was
        part = amount.point(lambda v, c=abs(change): v * c // 255)
        out.append(ImageChops.add(channel, part) if change >= 0 else ImageChops.subtract(channel, part))
    return Image.merge("RGBA", out + [a])


def render(project, kit=None):
    from PIL import Image, ImageDraw, ImageFont

    def font(size):
        for path in ("/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf",
                     "/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf"):
            if os.path.exists(path):
                return ImageFont.truetype(path, size)
        return ImageFont.load_default(size * 0.7)

    panels = json.load(open(os.path.join(project, "tabs.json"))).get("panels") or {}
    folder = os.path.join(project, "skin-images")
    written = []
    scale = 3
    for page, items in panels.items():
        image = Image.new("RGB", (WIDTH * scale, HEIGHT * scale), BACKGROUND)
        draw = ImageDraw.Draw(image)
        for item in items:
            colour = rgb(item["colour"])
            x0, y0, x1, y1 = (v * scale for v in box(item["cells"]))
            edge, band, line = mix(colour, 0.38), mix(colour, 0.26), scale + 1
            draw.rounded_rectangle([x0, y0, x1, y1], radius=12 * scale, fill=mix(colour, 0.075), outline=edge, width=line)
            draw.rounded_rectangle([x0, y0, x1, y0 + HEADING * scale], radius=12 * scale, fill=band, outline=edge, width=line)
            draw.rectangle([x0 + line, y0 + 18 * scale, x1 - line, y0 + HEADING * scale], fill=band)
            draw.text((x0 + 14 * scale, y0 + 5 * scale), item["title"], font=font(25 * scale), fill=mix(colour, 0.95, (255, 255, 255)))
        out = os.path.join(folder, "panel_%s.png" % page.replace(" ", "_"))
        image.resize((WIDTH, HEIGHT), Image.LANCZOS).save(out, optimize=True)
        written.append(out)
    if kit is None:
        found = glob.glob(os.path.join(project, "build", "*", "Plugin Skins", "knob_uni.png"))
        kit = os.path.dirname(found[0]) if found else None
    if kit:
        for colour in sorted({item["colour"] for items in panels.values() for item in items}):
            for strip in ("knob_uni", "knob_bi"):
                source = os.path.join(kit, strip + ".png")
                if os.path.isfile(source):
                    out = os.path.join(folder, "%s_%s.png" % (strip, colour))
                    tinted(Image.open(source), rgb(colour)).save(out, optimize=True)
                    written.append(out)
    return written


if __name__ == "__main__":
    for path in render(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "plugins", "deluge-synth"),
                       sys.argv[2] if len(sys.argv) > 2 else None):
        print("wrote", path)
