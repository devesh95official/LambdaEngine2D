#!/usr/bin/env python3
"""Writes assets/sprites.png for render_demo (standard library only: zlib + struct).

The sheet is 144x16 RGBA: eight 16x16 frames of a little walker (a light body that the demo
tints, bobbing over two stepping legs), then one 16x16 grass tile. Run it from anywhere:
    python3 examples/basics/07_render/make_assets.py
"""
import os
import struct
import zlib

W, H = 144, 16


def png(path, width, height, rows):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(row) for row in rows)
    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 9))
    data += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def main():
    px = [[0, 0, 0, 0] * W for _ in range(H)]

    def put(x, y, r, g, b, a=255):
        px[y][4 * x:4 * x + 4] = [r, g, b, a]

    for f in range(8):
        ox = 16 * f
        bob = 1 if f % 4 in (1, 2) else 0
        for y in range(2 + bob, 11 + bob):
            for x in range(4, 12):
                edge = x in (4, 11) or y in (2 + bob, 10 + bob)
                shade = 200 if edge else 245
                put(ox + x, y, shade, shade, shade)
        put(ox + 6, 5 + bob, 20, 20, 30)
        put(ox + 9, 5 + bob, 20, 20, 30)
        step = f if f < 4 else 7 - f
        for y in range(11 + bob, 16):
            put(ox + 5 + step // 2, y, 70, 50, 40)
            put(ox + 10 - step // 2, y, 70, 50, 40)
    for y in range(16):
        for x in range(16):
            blade = (x * 7 + y * 3) % 11 == 0
            put(128 + x, y, 90 if blade else 46, 150 if blade else 104, 70 if blade else 52)

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets", "sprites.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    png(out, W, H, px)
    print("wrote", out)


if __name__ == "__main__":
    main()
