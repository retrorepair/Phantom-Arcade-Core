#!/usr/bin/env python3
"""analyse_tearing.py - look for torn frames in a tools/mister_burst.py capture.

For each frame after the first, every row is compared with the same row of the frame before it. A clean new frame has
changed rows all the way down (or, in a static scene, none). A torn one has a boundary: above some row the content is new,
below it the previous frame's, or the other way round. The row of that boundary is reported.

What the numbers mean (see mister_burst.py): a read that races the FPGA's write tears anywhere; a display that is
being written under, or a write that lands mid-scan, tears at rows that repeat. So the useful output is the histogram
of boundary rows - a spread is the capture's own noise, a few tall spikes are the display.
"""
import struct
import sys
from collections import Counter

path = sys.argv[1]
data = open(path, "rb").read()
magic, w, h, n = struct.unpack_from("<4sIII", data, 0)
assert magic == b"BRST"
row = w * 3
frames = []
off = 16
for i in range(n):
    t = struct.unpack_from("<d", data, off)[0]
    off += 8
    frames.append((t, data[off:off + row * h]))
    off += row * h


def changed_rows(a, b):
    out = []
    for y in range(h):
        out.append(a[y * row:(y + 1) * row] != b[y * row:(y + 1) * row])
    return out


print("%d frames, %dx%d; mean spacing %.1f ms" % (n, w, h, (frames[-1][0] - frames[0][0]) * 1000.0 / max(1, n - 1)))
boundaries = Counter()
torn = clean = static = 0
detail = []
for i in range(1, n):
    ch = changed_rows(frames[i - 1][1], frames[i][1])
    nch = sum(ch)
    if nch == 0:
        static += 1
        continue
    # the number of times the changed/unchanged state flips going down the frame, ignoring tiny isolated runs
    runs = []
    cur, length = ch[0], 0
    for v in ch:
        if v == cur:
            length += 1
        else:
            runs.append((cur, length))
            cur, length = v, 1
    runs.append((cur, length))
    big = [(v, l) for v, l in runs if l >= 8]
    flips = sum(1 for k in range(1, len(big)) if big[k][0] != big[k - 1][0])
    if flips >= 1 and nch < h - 8:
        torn += 1
        y = 0
        for v, l in runs:
            if l >= 8 and y > 0:
                boundaries[(y // 8) * 8] += 1
            y += l
        detail.append((i, nch, [(("changed" if v else "same"), l) for v, l in runs if l >= 8]))
    else:
        clean += 1

print("unchanged from the previous capture: %d; changed all the way down: %d; changed over part of the frame: %d" % (static, clean, torn))
print("rows where changed/unchanged boundaries fall (8-row buckets), most common first:")
for y, c in boundaries.most_common(14):
    print("  rows %3d-%3d : %d" % (y, y + 7, c))
print("first few partly-changed captures:")
for d in detail[:8]:
    print("  capture %d: %d of %d rows changed; runs %s" % (d[0], d[1], h, d[2][:6]))
