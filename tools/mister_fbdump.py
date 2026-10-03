#!/usr/bin/env python3
"""mister_fbdump.py - read the Groovy core's framebuffer out of DDR, on the MiSTer.

Run this on the DE10-Nano itself. The HPS and the FPGA share the board's DDR3, so what
the core is displaying can be read straight out of memory - no capture card, no camera,
and it works while the core is running.

  python3 mister_fbdump.py out.ppm [width] [height]

Defaults to the launcher's 720x480. The pixels are BGR888 at BASEADDR + HEADER_OFFSET,
matching support/groovy/groovy.cpp:

    #define BASEADDR 0x30000000
    #define HEADER_LEN 0xff
    #define CHUNK 7
    #define HEADER_OFFSET HEADER_LEN - CHUNK      -> 248

STRICT_DEVMEM blocks read()/dd on /dev/mem but permits mmap, so this maps the window
rather than seeking it.
"""
import mmap
import os
import sys

BASEADDR = 0x30000000
HEADER_OFFSET = 0xFF - 7

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "fb.ppm"
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 720
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 480

    nbytes = w * h * 3
    pagesize = mmap.PAGESIZE
    start = BASEADDR & ~(pagesize - 1)
    skip = BASEADDR - start
    length = skip + HEADER_OFFSET + nbytes
    length = (length + pagesize - 1) & ~(pagesize - 1)

    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    try:
        mm = mmap.mmap(fd, length, mmap.MAP_SHARED, mmap.PROT_READ, offset=start)
    finally:
        os.close(fd)

    base = skip + HEADER_OFFSET
    raw = mm[base:base + nbytes]
    mm.close()

    # BGR888 in DDR -> RGB for the PPM
    px = bytearray(nbytes)
    px[0::3] = raw[2::3]
    px[1::3] = raw[1::3]
    px[2::3] = raw[0::3]

    with open(out, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        f.write(bytes(px))

    nonzero = sum(1 for i in range(0, nbytes, 997) if raw[i])
    print("wrote %s  %dx%d  (sampled non-zero bytes: %d)" % (out, w, h, nonzero))


if __name__ == "__main__":
    main()
