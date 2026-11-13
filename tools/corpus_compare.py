#!/usr/bin/env python3
"""Compare two runs of tests/engine_corpus.cpp preset by preset.

    tools/corpus_compare.py <reference.txt> <other.txt> [-v]

Each run prints one line per preset ending in a checksum of every output sample, so two builds
that produce the same audio produce the same line. Prints how many presets match and lists the
ones that do not (all of them with -v). Exit status 1 if any differ or are missing.
"""
import re
import sys


def load(path):
    out = {}
    for line in open(path, errors="replace"):
        m = re.match(r"PRESET (.*?): (.*)", line)
        if m:
            out[m.group(1)] = m.group(2).strip()
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if len(args) != 2:
        sys.exit(__doc__)
    a, b = load(args[0]), load(args[1])
    same = [k for k in a if b.get(k) == a[k]]
    differ = sorted(k for k in a if k in b and b[k] != a[k])
    missing = sorted(k for k in a if k not in b)
    print("%d presets in the reference, %d in the other: %d identical, %d differ, %d missing"
          % (len(a), len(b), len(same), len(differ), len(missing)))
    for k in differ if "-v" in sys.argv else differ[:10]:
        print("  " + k)
        print("      reference: " + a[k])
        print("      other:     " + b[k])
    for k in missing[:10]:
        print("  missing: " + k)
    sys.exit(1 if differ or missing else 0)


if __name__ == "__main__":
    main()
