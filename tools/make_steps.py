#!/usr/bin/env python3
"""Draw the sixteen step buttons of Downrush's SC STEPS page (the sidechain pattern, one bar
of sixteenths): plugins/deluge-synth/skin-images/step_N_{off,on}.png, checked in. tabs.json
("buttons") places them in one row; the first step of each beat is a little lighter when off.

    python3 tools/make_steps.py        (needs Pillow; only to change how they look)
"""
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, "plugins", "deluge-synth", "skin-images")
W, H = 68, 148          # the button's size on screen; tabs.json's bounds less a 6 px inset
SCALE = 4
PANEL = (0x2c, 0x2b, 0x2c, 255)
BEAT = (0x45, 0x44, 0x46, 255)    # off, on a beat
ACCENT = (0xd3, 0x11, 0x45, 255)  # the mpc-red kit's accent
WHITE = (255, 255, 255, 255)
DIM = (0x9a, 0x9a, 0x9e, 255)


def font(size):
    for path in ("/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf",
                 "/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf"):
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default(size * 0.7)


for step in range(1, 17):
    on_beat = step % 4 == 1
    for state, fill, ink in (("off", BEAT if on_beat else PANEL, DIM), ("on", ACCENT, WHITE)):
        image = Image.new("RGBA", (W * SCALE, H * SCALE), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        draw.rounded_rectangle([0, 0, W * SCALE - 1, H * SCALE - 1], radius=10 * SCALE, fill=fill)
        text = str(step)
        f = font(34 * SCALE)
        box = draw.textbbox((0, 0), text, font=f)
        draw.text(((W * SCALE - (box[2] - box[0])) / 2 - box[0], (H * SCALE - (box[3] - box[1])) / 2 - box[1]), text, font=f, fill=ink)
        image.resize((W, H), Image.LANCZOS).save(os.path.join(OUT, "step_%d_%s.png" % (step, state)))
print("wrote 32 images to", OUT)
