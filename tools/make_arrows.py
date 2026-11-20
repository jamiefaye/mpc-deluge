#!/usr/bin/env python3
"""Draw the up and down arrow buttons of Downrush's MAIN page (previous / next folder and
preset): plugins/deluge-synth/skin-images/arrow_{up,down}_{off,on}.png, checked in. tools/build.py copies
that folder into the built skin and tabs.json ("buttons") says which parameters use them.

    python3 tools/make_arrows.py        (needs Pillow; only to change how they look)
"""
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, "plugins", "deluge-synth", "skin-images")
W, H = 93, 68           # the button's size on screen; tabs.json's bounds less a 6 px inset
SCALE = 4               # drawn large and reduced, for smooth edges
PANEL = (0x2c, 0x2b, 0x2c, 255)   # a little lighter than the page background (1e1d1e)
ACCENT = (0xd3, 0x11, 0x45, 255)  # the mpc-red kit's accent
WHITE = (255, 255, 255, 255)

for direction in ("up", "down"):
    for count in (1, 2):   # one arrow: a step of one; two, one above the other: a step of ten
        for state, fill in (("off", PANEL), ("on", ACCENT)):
            image = Image.new("RGBA", (W * SCALE, H * SCALE), (0, 0, 0, 0))
            draw = ImageDraw.Draw(image)
            draw.rounded_rectangle([0, 0, W * SCALE - 1, H * SCALE - 1], radius=12 * SCALE, fill=fill)
            half, tall = (20 * SCALE, 22 * SCALE) if count == 1 else (18 * SCALE, 15 * SCALE)
            cx = W * SCALE / 2
            for k in range(count):
                cy = H * SCALE / 2 + (k - (count - 1) / 2) * (tall + 4 * SCALE)
                tip = cy - tall / 2 if direction == "up" else cy + tall / 2
                base = cy + tall / 2 if direction == "up" else cy - tall / 2
                draw.polygon([(cx, tip), (cx + half, base), (cx - half, base)], fill=WHITE)
            name = "arrow_%s%s_%s.png" % (direction, "" if count == 1 else "2", state)
            image.resize((W, H), Image.LANCZOS).save(os.path.join(OUT, name))
print("wrote eight images to", OUT)
