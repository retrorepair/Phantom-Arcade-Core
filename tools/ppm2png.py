#!/usr/bin/env python3
"""ppm2png.py - minimal binary-PPM (P6) to PNG converter.

Exists so phantom_preview's output can be looked at without pulling in Pillow;
zlib and struct are stdlib. Optional --scale N nearest-neighbour upscales, and
--par corrects the 720x480 framebuffer to a 4:3 display so the preview has the
same proportions the CRT shows (720 non-square pixels are not 3:2).

  python3 ppm2png.py in.ppm out.png [--scale 1] [--par]
"""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit("not a binary PPM (P6)")
    # header: P6 <w> <h> <maxval>, whitespace separated, '#' comments allowed
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1  # single whitespace byte after maxval
    w, h, _maxval = fields
    return w, h, data[i : i + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def resample(w, h, rgb, nw, nh):
    out = bytearray(nw * nh * 3)
    for y in range(nh):
        sy = y * h // nh
        for x in range(nw):
            sx = x * w // nw
            s = (sy * w + sx) * 3
            d = (y * nw + x) * 3
            out[d : d + 3] = rgb[s : s + 3]
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    scale, par = 1, False
    args = sys.argv[3:]
    for n, a in enumerate(args):
        if a == "--scale" and n + 1 < len(args):
            scale = int(args[n + 1])
        elif a == "--par":
            par = True

    w, h, rgb = read_ppm(src)
    nw, nh = w, h
    if par:
        nw = h * 4 // 3  # 480 * 4/3 = 640, the CRT's real proportions
    nw *= scale
    nh *= scale
    if (nw, nh) != (w, h):
        rgb = resample(w, h, rgb, nw, nh)
    write_png(dst, nw, nh, rgb)
    print(f"{dst} {nw}x{nh}")


if __name__ == "__main__":
    main()
