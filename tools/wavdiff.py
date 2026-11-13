#!/usr/bin/env python3
"""Compare two recordings from tests/engine_smoke.cpp (32-bit stereo WAV) sample by sample.

    tools/wavdiff.py a.wav b.wav

Prints whether they are identical and, if not, where they first differ (frame, time, and the
128-frame render block), how many samples differ and by how much.
"""
import struct
import sys


def read(path):
    data = open(path, "rb").read()
    i = data.index(b"data") + 8
    n = (len(data) - i) // 4
    return struct.unpack("<%di" % n, data[i:i + n * 4])


a, b = read(sys.argv[1]), read(sys.argv[2])
n = min(len(a), len(b))
bad = [i for i in range(n) if a[i] != b[i]]
if not bad and len(a) == len(b):
    print("identical: %d samples" % n)
    sys.exit(0)
first = bad[0] if bad else n
print("%d of %d samples differ (lengths %d, %d)" % (len(bad), n, len(a), len(b)))
print("first difference at frame %d (%.3f s, render block %d, frame %d within it): %d vs %d"
      % (first // 2, first / 2 / 44100.0, first // 2 // 128, first // 2 % 128, a[first], b[first]))
print("largest difference %d" % max(abs(a[i] - b[i]) for i in bad))
sys.exit(1)
