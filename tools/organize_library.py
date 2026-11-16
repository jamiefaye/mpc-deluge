#!/usr/bin/env python3
"""Lay out a Deluge card's SYNTH presets, and the samples they use, as a library for Deluge Synth:
folders by collection instead of the card's one flat SYNTHS folder. Writes build/library/Content,
which `./deploy.sh plugins/deluge-synth --content build/library` installs in place of the
project's small Content/ (the one the tests use).

    tools/organize_library.py [card root, default ~/deluge/SDCard]

The folders, and how a preset is put in one (Jamie's card, 2026-11-16: 747 presets):
    Factory 000-099 ...   SYNTnnn, by hundreds; 300 and up together
    BOD 1 ... BOD 6       Boards of Deluge. Volumes 5 and 6 name themselves (BOC5, BOD6). The
                          numbered presets 1-211 before them are taken to be volumes 1-4, divided
                          where the files' dates divide (1-50, 51-150, 151-211); where volume 2
                          ends and 3 begins is not known, so they share a folder.
    Other                 everything else ("BASS - ...", "PADS - ...")
    User                  where the plugin's Save button writes; made empty here
These files belong to whoever owns the card, and the Boards of Deluge collections are bought:
build/ is not tracked by git, and nothing here goes in a package for anyone else.
"""
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
card = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/deluge/SDCard")
out = os.path.join(HERE, "build", "library")


def folder_of(name):
    stem = name[:-4]
    m = re.match(r"(?i)SYNT(\d{3})", stem)
    if m:
        hundred = int(m.group(1)) // 100
        return "Factory 300+" if hundred >= 3 else "Factory %d00-%d99" % (hundred, hundred)
    if re.match(r"BOD6-", stem):
        return "BOD 6"
    if re.match(r"\d+-BOC5-", stem):
        return "BOD 5"
    m = re.match(r"(\d+) ?- ?", stem)
    if m:
        n = int(m.group(1))
        return "BOD 1" if n <= 50 else "BOD 2-3" if n <= 150 else "BOD 4"
    return "Other"


presets = sorted(f for f in os.listdir(os.path.join(card, "SYNTHS")) if f.lower().endswith(".xml"))
groups = {}
for name in presets:
    groups.setdefault(folder_of(name), []).append(name)
shutil.rmtree(out, ignore_errors=True)
os.makedirs(os.path.join(out, "Content", "SYNTHS", "User"))
for folder in sorted(groups):
    subprocess.run([sys.executable, os.path.join(HERE, "tools", "bundle_presets.py"), card, out, folder] + groups[folder],
                   check=True, stdout=subprocess.DEVNULL)
    print("%-18s %4d presets" % (folder, len(groups[folder])))
samples = sum(len(files) for _, _, files in os.walk(os.path.join(out, "Content", "SAMPLES")))
size = sum(os.path.getsize(os.path.join(d, f)) for d, _, files in os.walk(os.path.join(out, "Content")) for f in files)
print("%d presets in %d folders, %d sample files, %.0f MB, in %s" % (len(presets), len(groups), samples, size / 1e6,
                                                                    os.path.relpath(out, HERE)))
