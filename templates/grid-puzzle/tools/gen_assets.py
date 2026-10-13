#!/usr/bin/env python3
"""Writes the template's placeholder art and sound (standard library only: zlib, struct, math).

    python3 tools/gen_assets.py          # rewrites assets/sprites/tiles.png and assets/sfx/push.wav

The output is deterministic (no randomness), so running it again gives identical files.
TODO(art): replace the outputs with real art. Keep the frame order of tiles.png (main.cpp, enum
Frame) or change both together: floor, wall, goal, crate, crate on goal, player; 16x16 each.
"""
import math
import os
import struct
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
T = 16  # tile size in pixels (kTile in rules.h)


def write_png(path, width, height, pixels):
    """pixels: a list of rows, each a list of (r, g, b, a)."""
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


def tile(fill):
    return [[fill(x, y) for x in range(T)] for y in range(T)]


def floor(x, y):
    edge = x == 0 or y == 0
    return (46, 42, 54, 255) if edge else (58, 53, 68, 255)


def wall(x, y):
    row = y // 4
    mortar = y % 4 == 3 or (x + (4 if row % 2 else 0)) % 8 == 7
    if mortar:
        return (64, 44, 40, 255)
    return (150, 96, 72, 255) if y % 4 == 0 else (124, 78, 60, 255)


def goal(x, y):
    d = abs(x - 7.5) + abs(y - 7.5)  # a diamond outline on the floor
    return (232, 196, 84, 255) if 4.0 <= d <= 5.5 else floor(x, y)


def crate(body, edge):
    def fill(x, y):
        if x in (0, 15) or y in (0, 15):
            return (0, 0, 0, 0)
        if x in (1, 14) or y in (1, 14):
            return edge
        if x == y or x == 15 - y:  # the X brace
            return edge
        return body
    return fill


def player(x, y):
    head = (x - 7.5) ** 2 + (y - 5) ** 2 <= 10
    body = 4 <= x <= 11 and 9 <= y <= 14
    if head:
        return (40, 30, 30, 255) if y == 5 and x in (6, 9) else (240, 208, 170, 255)
    if body:
        return (70, 150, 220, 255) if y < 13 else (40, 90, 150, 255)
    return (0, 0, 0, 0)


def write_tiles():
    frames = [tile(floor), tile(wall), tile(goal),
              tile(crate((176, 120, 64, 255), (104, 66, 34, 255))),
              tile(crate((120, 186, 92, 255), (52, 104, 48, 255))),
              tile(player)]
    rows = [[px for f in frames for px in f[y]] for y in range(T)]
    write_png(os.path.join(ROOT, "sprites", "tiles.png"), T * len(frames), T, rows)


def write_push_wav():
    """A short wooden thud: a falling sine with a little deterministic noise, 16-bit mono."""
    rate, seconds = 22050, 0.14
    n = int(rate * seconds)
    seed, phase, samples = 12345, 0.0, []
    for i in range(n):
        t = i / n
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        noise = seed / 0x3FFFFFFF - 1.0
        phase += 2 * math.pi * (170 - 110 * t) / rate
        v = (1 - t) ** 2 * (0.75 * math.sin(phase) + 0.25 * noise * (1 - t) ** 2)
        samples.append(int(max(-1.0, min(1.0, v)) * 30000))
    data = struct.pack("<%dh" % n, *samples)
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    header += b"data" + struct.pack("<I", len(data))
    path = os.path.join(ROOT, "sfx", "push.wav")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header + data)


if __name__ == "__main__":
    write_tiles()
    write_push_wav()
    print("wrote assets/sprites/tiles.png and assets/sfx/push.wav")
