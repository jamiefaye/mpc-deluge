#!/usr/bin/env python3
"""Copy Deluge SYNTH presets, and the sample files they use, from a Deluge SD card (or a copy of
one) into a plugin project's Content/ folder, keeping the card's layout.

    tools/bundle_presets.py <card root> <plugin project> <subfolder under SYNTHS> <preset>...

<preset> is a file name under the card's SYNTHS folder, with or without ".XML"; shell wildcards
work if quoted. Presets land in Content/SYNTHS/<subfolder>/, samples in Content/SAMPLES/... at the
paths the presets name. Content/ is not tracked by git: these files belong to whoever owns the card.
"""
import glob
import os
import re
import shutil
import sys

card, project, subfolder = sys.argv[1], sys.argv[2], sys.argv[3]
presets = []
for pattern in sys.argv[4:]:
    if not pattern.lower().endswith(".xml"):
        pattern += ".XML"
    found = sorted(glob.glob(os.path.join(glob.escape(card), "SYNTHS", pattern)))
    if not found:
        print("no preset matches " + pattern)
    presets += found
copied = missing = 0
size = 0
for preset in presets:
    dest = os.path.join(project, "Content", "SYNTHS", subfolder, os.path.basename(preset))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copy(preset, dest)
    text = open(preset, encoding="utf-8", errors="replace").read()
    names = set(re.findall(r'fileName="([^"]+)"', text)) | set(re.findall(r"<fileName>([^<]+)</fileName>", text))
    for name in sorted(n.strip().replace("&amp;", "&") for n in names if n.strip()):
        src = os.path.join(card, name)
        if not os.path.isfile(src):
            print("  %s: sample not on the card: %s" % (os.path.basename(preset), name))
            missing += 1
            continue
        out = os.path.join(project, "Content", name)
        if not os.path.exists(out):
            os.makedirs(os.path.dirname(out), exist_ok=True)
            shutil.copy(src, out)
            copied += 1
            size += os.path.getsize(out)
print("%d presets into Content/SYNTHS/%s, %d sample files copied (%.1f MB), %d missing" % (
    len(presets), subfolder, copied, size / 1e6, missing))
