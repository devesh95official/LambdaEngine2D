#!/usr/bin/env python3
"""Writes assets/fx/ember.png for particles_demo: an 8x8 soft white dot (RGBA).
Standard library only (zlib + struct), so it runs anywhere python3 does:  python3 make_assets.py"""
import math
import os
import struct
import zlib


def png(path, width, height, pixels):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    rows = b"".join(b"\x00" + bytes(pixels[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def ember(size=8):
    out = []
    c = (size - 1) / 2.0
    for y in range(size):
        for x in range(size):
            d = math.hypot(x - c, y - c) / (size / 2.0)
            a = max(0.0, 1.0 - d) ** 1.5
            out += [255, 255, 255, int(round(255 * a))]
    return out


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(os.path.join(here, "assets", "fx"), exist_ok=True)
    png(os.path.join(here, "assets", "fx", "ember.png"), 8, 8, ember())
