#!/usr/bin/env python3
"""mister_burst.py - capture a burst of the Groovy core's framebuffer, as fast as DDR allows. Run on the MiSTer.

  python3 mister_burst.py out.bin [width] [height] [frames]

Writes a tiny header (magic, width, height, count) followed by `count` frames of BGR888 and, after each, the monotonic
time it was read at. tools/analyse_tearing.py (PC side) reads it back and looks for frames that are part one picture and
part another - which is what a frame written while the display is reading it looks like.

The mapping is uncached (/dev/mem with O_SYNC), so a frame takes a few milliseconds to read; the burst is therefore a
sample of the stream, not every frame, and a frame the FPGA writes *while* it is being read here shows as torn in the
capture whether or not it was torn on the screen. The analysis accounts for that by looking at where the tears fall:
a read racing a write puts them anywhere, a display race puts them at the same few rows again and again.
"""
import mmap
import os
import struct
import sys
import time

BASEADDR = 0x30000000
HEADER_OFFSET = 0xFF - 7


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "burst.bin"
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 640
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 448
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 60

    nbytes = w * h * 3
    page = mmap.PAGESIZE
    start = BASEADDR & ~(page - 1)
    skip = BASEADDR - start
    length = (skip + HEADER_OFFSET + nbytes + page - 1) & ~(page - 1)
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    mm = mmap.mmap(fd, length, mmap.MAP_SHARED, mmap.PROT_READ, offset=start)
    base = skip + HEADER_OFFSET

    frames = []
    t0 = time.monotonic()
    for _ in range(count):
        t = time.monotonic()
        frames.append((t - t0, mm[base:base + nbytes]))
    t1 = time.monotonic()
    mm.close()
    os.close(fd)

    with open(out, "wb") as f:
        f.write(struct.pack("<4sIII", b"BRST", w, h, len(frames)))
        for t, data in frames:
            f.write(struct.pack("<d", t))
            f.write(data)
    print("captured %d frames of %dx%d in %.2f s (%.1f ms each) -> %s" % (len(frames), w, h, t1 - t0, (t1 - t0) * 1000.0 / len(frames), out))


if __name__ == "__main__":
    main()
