#!/usr/bin/env python3
"""Writes the roguelike template's placeholder art and sound (standard library only: no PIL/numpy).

    python3 tools/gen_assets.py        # from the game folder; rewrites the files below in place

Outputs (small and committed, so a build never needs Python):
  assets/sprites/actors.png    16x16 glyphs, 2 frames each (idle bob): '@' player, 'g' monster
  assets/sprites/actors.json   Aseprite sheet (array form); one frame tag per glyph = Animator clip
  assets/sfx/hit.wav           a short 16-bit mono thump, the "hit" group of assets/sfx/game.sounds.json

Hand-written data lives next to them: assets/prefabs.json, assets/sfx/game.sounds.json.
TODO(art): replace with real art. Any Aseprite export (File > Export Sprite Sheet, JSON array,
tags on) works as a sheet; keep the tag names the prefabs use as clips.
"""
import json
import math
import os
import struct
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
T = 16  # glyph cell size in pixels

# 5x7 bitmaps, '#' = ink. TODO(art): add a glyph per new monster or item kind.
GLYPHS = [  # (frame tag = clip name, bitmap, colour)
    ("player", [".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".####"], (255, 236, 140)),
    ("monster", [".....", ".####", "#...#", "#...#", ".####", "....#", ".###."], (120, 210, 90)),
]


def write_png(path, width, height, pixels):
    """pixels: rows of (r, g, b, a). Filter 0 on every row, one zlib stream."""
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9))
                + chunk(b"IEND", b""))


def draw_glyph(img, ox, oy, bits, color, lift):
    """The 5x7 bitmap at 2x (10x14) centred in a cell, with a 1-pixel dark outline."""
    x0, y0 = ox + 3, oy + 1 - lift
    ink = [(x0 + gx * 2 + sx, y0 + gy * 2 + sy)
           for gy, row in enumerate(bits) for gx, ch in enumerate(row) if ch == "#"
           for sy in (0, 1) for sx in (0, 1)]
    for (x, y) in ink:  # outline first, ink on top
        for (dx, dy) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if ox <= x + dx < ox + T and oy <= y + dy < oy + T:
                img[y + dy][x + dx] = (10, 8, 14, 255)
    for (x, y) in ink:
        img[y][x] = color + (255,)


def make_sheet():
    frames, tags = [], []
    width, height = T * 2 * len(GLYPHS), T
    img = [[(0, 0, 0, 0)] * width for _ in range(height)]
    for i, (tag, bits, color) in enumerate(GLYPHS):
        for f in range(2):  # frame 1: one pixel higher and a little brighter
            n = i * 2 + f
            bright = tuple(min(255, int(c * (1.0 + 0.15 * f))) for c in color)
            draw_glyph(img, n * T, 0, bits, bright, f)
            frames.append({"filename": "%s %d" % (tag, f), "frame": {"x": n * T, "y": 0, "w": T, "h": T},
                           "rotated": False, "trimmed": False,
                           "spriteSourceSize": {"x": 0, "y": 0, "w": T, "h": T},
                           "sourceSize": {"w": T, "h": T}, "duration": 450 if f == 0 else 350})
        tags.append({"name": tag, "from": i * 2, "to": i * 2 + 1, "direction": "forward"})
    os.makedirs(os.path.join(ROOT, "sprites"), exist_ok=True)
    write_png(os.path.join(ROOT, "sprites", "actors.png"), width, height, img)
    sheet = {"frames": frames,
             "meta": {"app": "tools/gen_assets.py", "version": "1", "image": "actors.png", "format": "RGBA8888",
                      "size": {"w": width, "h": height}, "scale": "1", "frameTags": tags, "slices": []}}
    with open(os.path.join(ROOT, "sprites", "actors.json"), "w") as f:
        json.dump(sheet, f, indent=1)
        f.write("\n")


def make_hit_wav():
    """0.14 s: a falling square tone plus decaying noise (deterministic LCG, so reruns are identical)."""
    rate, seconds = 22050, 0.14
    n = int(rate * seconds)
    seed, samples, phase = 12345, [], 0.0
    for i in range(n):
        t = i / n
        phase += (330.0 - 240.0 * t) / rate
        tone = 1.0 if (phase % 1.0) < 0.5 else -1.0
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        noise = seed / 0x3FFFFFFF - 1.0
        env = math.exp(-5.0 * t) * min(1.0, i / 40.0)  # 40-sample attack: no click
        samples.append(int(max(-1.0, min(1.0, (0.45 * tone + 0.35 * noise) * env)) * 30000))
    data = struct.pack("<%dh" % n, *samples)
    os.makedirs(os.path.join(ROOT, "sfx"), exist_ok=True)
    with open(os.path.join(ROOT, "sfx", "hit.wav"), "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))  # PCM, mono, 16-bit
        f.write(b"data" + struct.pack("<I", len(data)) + data)


if __name__ == "__main__":
    make_sheet()
    make_hit_wav()
    print("assets written to", os.path.normpath(ROOT))
