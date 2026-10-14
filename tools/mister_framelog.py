#!/usr/bin/env python3
"""mister_framelog.py - log the frame counter the HPS hands the Groovy core, to see how steadily frames arrive. Run on the MiSTer.

  python3 mister_framelog.py [seconds] [header-offset: 32 for LZ4/NLC (default), 0 for raw]

The core's DDR window starts with the blit header: frame number (3 bytes), pixel or compressed-byte count (3), blit count (2),
at byte 0 for a raw stream and byte 32 for a compressed one.
The HPS rewrites it once per received frame, so polling it fast enough gives the arrival time of every frame and the
step in the frame number between them. A steady 60 Hz stream steps by 1 every ~16.7 ms; a PC clock that drifts against
the CRT's shows as an occasional step of 2 (a frame the display never showed) or a long gap (one it showed twice), and
a network or encode stall shows as a gap well over 20 ms. Prints the histogram of both, then the worst gaps.
"""
import mmap
import os
import struct
import sys
import time
from collections import Counter

BASEADDR = 0x30000000


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
    # raw (uncompressed) blits write the header at byte 0; the compressed (LZ4 / NLC) ones at byte 32
    OFFSET = int(sys.argv[2]) if len(sys.argv) > 2 else 32
    page = mmap.PAGESIZE
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    mm = mmap.mmap(fd, page, mmap.MAP_SHARED, mmap.PROT_READ, offset=BASEADDR)

    seen = []  # (time, frame, pixels, blits)
    last = None
    t0 = time.monotonic()
    while True:
        now = time.monotonic()
        if now - t0 > seconds:
            break
        h = mm[0:40]
        o = OFFSET
        frame = h[o] | (h[o + 1] << 8) | (h[o + 2] << 16)
        pixels = h[o + 3] | (h[o + 4] << 8) | (h[o + 5] << 16)
        blits = h[o + 6] | (h[o + 7] << 8)
        key = frame  # the pixel and blit counts also change within a frame, so key on the frame number alone
        if key != last:
            seen.append((now - t0, frame, pixels, blits))
            last = key
    mm.close()
    os.close(fd)

    n = len(seen)
    print("%d header changes in %.1f s (%.2f per second)" % (n, seconds, n / seconds))
    if n < 3:
        return
    steps = Counter()
    gaps = []
    for a, b in zip(seen, seen[1:]):
        steps[b[1] - a[1]] += 1
        gaps.append(((b[0] - a[0]) * 1000.0, a[1], b[1]))
    print("frame-number step between consecutive header changes:")
    for s, c in sorted(steps.items(), key=lambda kv: -kv[1])[:8]:
        print("  step %+d : %d" % (s, c))
    buckets = Counter()
    for g, _, _ in gaps:
        if g < 8:
            buckets["<8"] += 1
        elif g < 14:
            buckets["8-14"] += 1
        elif g < 20:
            buckets["14-20 (one frame)"] += 1
        elif g < 26:
            buckets["20-26"] += 1
        elif g < 40:
            buckets["26-40"] += 1
        else:
            buckets[">=40"] += 1
    print("gap between header changes (ms):")
    for k in ["<8", "8-14", "14-20 (one frame)", "20-26", "26-40", ">=40"]:
        print("  %-18s %d" % (k, buckets.get(k, 0)))
    print("worst gaps (ms, frame a -> b):")
    for g, a, b in sorted(gaps, reverse=True)[:6]:
        print("  %.1f  %d -> %d" % (g, a, b))
    mean = sum(g for g, _, _ in gaps) / len(gaps)
    print("mean gap %.2f ms (a 59.94 Hz stream is 16.68)" % mean)


if __name__ == "__main__":
    main()
