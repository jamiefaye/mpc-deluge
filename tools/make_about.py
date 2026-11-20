#!/usr/bin/env python3
"""Draw Downrush's ABOUT page (the credits) as a picture: skin-images/about.png, checked in,
from the "about" lines in the plugin's tabs.json and the version in its plugin.json.

A picture because a skin's fixed-text labels do not show on a Force: the credits were first built
from them and the page came up empty (2026-11-18), as the segment buttons' captions had before.
tools/build.py runs this at every build when Pillow is there, so the picture follows tabs.json.

    python3 tools/make_about.py [plugin folder]
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIDTH, HEIGHT = 1280, 628
BACKGROUND = (0x1e, 0x1d, 0x1e)


def render(project):
    from PIL import Image, ImageDraw, ImageFont

    def font(size, bold):
        for path in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf" if bold else "/System/Library/Fonts/Supplemental/Arial.ttf",
                     "/System/Library/Fonts/Helvetica.ttc"):
            if os.path.exists(path):
                return ImageFont.truetype(path, size)
        return ImageFont.load_default(size)

    about = json.load(open(os.path.join(project, "tabs.json")))["about"]
    version = json.load(open(os.path.join(project, "plugin.json")))["version"]
    lines = []
    for line in about["lines"]:
        parts = line if isinstance(line, list) else [line]
        text = parts[0].replace("{version}", version)
        height = parts[1] if len(parts) > 1 else 24
        colour = parts[2] if len(parts) > 2 else "fff2f2f2"
        lines.append((text, height, tuple(int(colour[i:i + 2], 16) for i in (2, 4, 6))))
    total = sum(int(h * 1.5) for _, h, _ in lines)
    if total > HEIGHT - 20:
        sys.exit("about: the lines need %d px of the page's %d" % (total, HEIGHT))
    scale = 2
    image = Image.new("RGB", (WIDTH * scale, HEIGHT * scale), BACKGROUND)
    draw = ImageDraw.Draw(image)
    y = (HEIGHT - total) // 2
    for text, height, colour in lines:
        if text:
            f = font(int(height * 0.86 * scale), height >= 30)
            w = draw.textlength(text, font=f)
            if w > (WIDTH - 40) * scale:
                sys.exit("about: this line is too long for the page: %s" % text)
            draw.text(((WIDTH * scale - w) / 2, (y + height * 0.25) * scale), text, font=f, fill=colour)
        y += int(height * 1.5)
    out = os.path.join(project, "skin-images", "about.png")
    image.resize((WIDTH, HEIGHT), Image.LANCZOS).save(out, optimize=True)
    return out


if __name__ == "__main__":
    print("wrote", render(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "plugins", "deluge-synth")))
