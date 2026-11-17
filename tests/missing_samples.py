#!/usr/bin/env python3
"""A preset whose samples are not where it says must still find them, and say which it could not.

    tools/build_firmware.py native --test tests/engine_script.cpp
    tests/missing_samples.py <card root> <preset path under it, one that uses samples>

Makes four copies of the preset in a scratch folder and plays each through the engine:
  as named   the samples at the paths the preset gives: the reference
  case       folder and file names in the other case (a Deluge does not tell them apart)
  moved      every sample in one other folder, found by its name
  absent     no samples: every path must be reported missing, and nothing may fail
The first three must sound the same, sample for sample.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(HERE, "build", "firmware", "native", "engine_script")
card, preset = sys.argv[1], sys.argv[2]
text = open(os.path.join(card, preset), errors="replace").read()
names = sorted(set(re.findall(r'fileName="([^"]+)"', text) + re.findall(r"<fileName>([^<]+)</fileName>", text)))
names = [n for n in names if os.path.exists(os.path.join(card, n))]
if not names:
    sys.exit("that preset names no sample that is on the card")
work = os.path.join(HERE, "build", "missing-samples")


def play(kind, place):
    """place: sample path as named -> path to put it at, or None to leave it out."""
    root = os.path.join(work, kind)
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(os.path.join(root, "SYNTHS"))
    os.makedirs(os.path.join(root, "SAMPLES"), exist_ok=True)
    shutil.copy(os.path.join(card, preset), os.path.join(root, "SYNTHS", "P.XML"))
    for name in names:
        to = place(name)
        if to is not None:
            os.makedirs(os.path.dirname(os.path.join(root, to)), exist_ok=True)
            shutil.copy(os.path.join(card, name), os.path.join(root, to))
    raw = os.path.join(root, "take.raw")
    commands = "load|SYNTHS/P.XML\nsamples\non|60|100\non|48|100\nrender|500|%s\noff|60\noff|48\nrender|200|%s\n" % (raw, raw)
    done = subprocess.run([EXE, root], input=commands, capture_output=True, text=True, env={**os.environ, "DELUGE_SCRUB": "165"})
    if done.returncode != 0:
        sys.exit("%s: the engine stopped: %s" % (kind, done.stderr))
    counts = [line for line in done.stdout.splitlines() if line.startswith("SAMPLES|")][0]
    missing = [line.split("|", 1)[1] for line in done.stdout.splitlines() if line.startswith("MISSING|")]
    elsewhere = int(re.search(r"found elsewhere (\d+)", counts).group(1))
    return hashlib.sha1(open(raw, "rb").read()).hexdigest(), elsewhere, missing, counts


def swapcase_below_samples(name):
    head, _, rest = name.partition("/")
    return head + "/" + rest.swapcase()


failed = 0


def check(what, ok):
    global failed
    print("%s  %s" % ("PASS" if ok else "FAIL", what))
    failed += 0 if ok else 1


print("%s: %d sample files" % (preset, len(names)))
reference, elsewhere, missing, counts = play("as-named", lambda n: n)
check("as named: all found where the preset says (%s)" % counts, elsewhere == 0 and not missing)
sound, elsewhere, missing, counts = play("case", swapcase_below_samples)
check("names in the other case: nothing missing, same sound (%s)" % counts, not missing and sound == reference)
sound, elsewhere, missing, counts = play("moved", lambda n: "SAMPLES/Somewhere Else/" + os.path.basename(n))
check("all in another folder: every one found by name, same sound (%s)" % counts,
      not missing and elsewhere == len(names) and sound == reference)
sound, elsewhere, missing, counts = play("absent", lambda n: None)
check("no samples at all: each path reported missing (%s)" % counts, sorted(missing) == names and elsewhere == 0)
print("Result: %s" % ("PASS" if failed == 0 else "FAIL (%d)" % failed))
sys.exit(1 if failed else 0)
