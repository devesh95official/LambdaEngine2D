#!/usr/bin/env python3
"""Generates tilemap_demo's tileset image, assets/maps/tiles.png (8 tiles of 16 x 16 in one row).

Standard library only (zlib + struct; no Pillow). The PNG is committed; run this again only to
change the art:  python3 examples/basics/10_tilemap/make_tiles.py
Tiles (local ids): 0 floor, 1 wall, 2 water, 3 grass, 4-6 torch flame frames, 7 crate.
The map that uses them, assets/maps/dungeon.tmj, is written by hand (it opens in Tiled).
"""
import os
import struct
import zlib

T = 16
COLS = 8
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets", "maps", "tiles.png")


def png(width, height, rgba):
    rows = b"".join(b"\x00" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def texel(tile, x, y):
    noise = ((x * 7 + y * 13 + tile * 5) * 2654435761 >> 7) & 15
    if tile == 0:  # floor: grey stone with speckles
        v = 70 + noise
        return (v, v, v + 6, 255)
    if tile == 1:  # wall: bricks and mortar
        mortar = y % 8 == 7 or (x + (4 if (y // 8) % 2 else 0)) % 8 == 7
        return (60, 50, 45, 255) if mortar else (140 + noise, 70 + noise // 2, 50, 255)
    if tile == 2:  # water with ripples
        return (90, 140, 220, 255) if (x + y * 2) % 8 == 0 else (30, 70 + noise, 160 + noise, 255)
    if tile == 3:  # grass
        return (40 + noise, 120 + noise * 2, 40, 255)
    if tile in (4, 5, 6):  # a torch whose flame grows and shrinks (transparent around it)
        size = [4, 6, 5][tile - 4]
        dx, dy = x - 8, y - 7
        if dx * dx * 2 + dy * dy <= size * size:
            return (255, max(80, 220 - abs(dy) * 20), 40, 255)
        if 7 <= x <= 8 and y >= 10:
            return (90, 60, 30, 255)
        return (0, 0, 0, 0)
    if tile == 7:  # crate
        edge = x in (0, 15) or y in (0, 15) or x == y or x == 15 - y
        return (90, 60, 25, 255) if edge else (170, 120, 60, 255)
    return (255, 0, 255, 255)


def main():
    width, height = COLS * T, T
    rgba = []
    for y in range(height):
        for x in range(width):
            rgba.extend(texel(x // T, x % T, y))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(png(width, height, rgba))


if __name__ == "__main__":
    main()
