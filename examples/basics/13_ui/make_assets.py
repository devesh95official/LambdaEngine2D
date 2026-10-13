#!/usr/bin/env python3
"""Writes assets/ui/panel.png for ui_demo: a 12x12 white nine-slice frame (1 px rounded corners,
2 px light rim, grey centre) that the theme tints per widget. Standard library only (zlib +
struct), so it runs anywhere python3 does:  python3 make_assets.py"""
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


def panel(size=12):
    out = []
    last = size - 1
    for y in range(size):
        for x in range(size):
            corner = (x in (0, last)) and (y in (0, last))
            edge = min(x, y, last - x, last - y)
            if corner:
                out += [0, 0, 0, 0]                      # rounded corner
            elif edge == 0:
                out += [255, 255, 255, 255]              # rim
            elif edge == 1:
                out += [235, 235, 235, 255]
            else:
                out += [200, 200, 200, 255]              # centre (stretched)
    return out


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(os.path.join(here, "assets", "ui"), exist_ok=True)
    png(os.path.join(here, "assets", "ui", "panel.png"), 12, 12, panel())
