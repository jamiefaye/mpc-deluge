#!/usr/bin/env python3
"""List NEON loads and stores in 32-bit ARM code that require aligned addresses and whose address
does not obviously come from the stack or a compiler-placed constant. On 32-bit ARM such an access
faults if the address is not aligned (arm64 has no such fault, so the simulator cannot see it).

    tools/check_alignment.py <file.o | file.so> ...     needs arm-none-eabi-objdump

Heuristic: an access is accepted when the base register was last set by "add rX, sp|pc|fp, ..."
within the preceding 40 instructions. Everything else is printed for a human to judge.
"""
import re
import subprocess
import sys

ACCESS = re.compile(r"\b(v(?:ld|st)[1-4]\S*)\s.*\[(\w+) :(\d+)\]")
SETS = re.compile(r"\b(?:add|sub|mov)\w*\s+(\w+), (\w+)")

bad = 0
for path in sys.argv[1:]:
    out = subprocess.run(["arm-none-eabi-objdump", "-d", path], capture_output=True, text=True).stdout
    lines = out.splitlines()
    func = "?"
    for i, line in enumerate(lines):
        if line.endswith(">:"):
            func = line.split("<", 1)[1][:-2]
        m = ACCESS.search(line)
        if not m:
            continue
        base = m.group(2)
        ok = base in ("sp", "fp")
        for prev in reversed(lines[max(0, i - 40):i]):
            s = SETS.search(prev)
            if s and s.group(1) == base:
                ok = s.group(2) in ("sp", "pc", "fp", "r11")
                break
        if not ok:
            bad += 1
            print("%s: %s  [%s :%s]  in %s" % (path.split("/")[-1], m.group(1), base, m.group(3), func[:90]))
print("%d access(es) need a look" % bad)
sys.exit(1 if bad else 0)
