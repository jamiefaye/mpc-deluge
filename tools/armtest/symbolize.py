#!/usr/bin/env python3
"""Turn the CRASH-PC / CRASH-LR / CRASH-STACK lines a test program printed (tests/crash_report.h)
into function names.   tools/armtest/symbolize.py <program> [console log]

The stack lines are every code address found on the stack, so the list contains the real call
chain plus stale leftovers; read it as a hint, innermost first.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
exe = sys.argv[1]
log = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "build", "armtest", "console.log")
addrs = re.findall(r"^(CRASH-(?:PC|LR|STACK)) (0x[0-9a-f]+)", open(log).read(), re.M)
if not addrs:
    sys.exit("no CRASH- lines in " + log)
out = subprocess.run(["arm-none-eabi-addr2line", "-f", "-C", "-e", exe] + [a for _, a in addrs],
                     capture_output=True, text=True).stdout.splitlines()
seen = set()
for i, (kind, addr) in enumerate(addrs):
    func, where = out[2 * i], out[2 * i + 1]
    key = (func,)
    if kind == "CRASH-STACK" and key in seen:
        continue
    seen.add(key)
    print("%-12s %s  %s  %s" % (kind[6:], addr, func[:110], where.replace(HERE + "/", "")[-70:]))
