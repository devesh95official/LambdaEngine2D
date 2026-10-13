#!/usr/bin/env python3
"""Generates the shooter's assets (stdlib only: zlib + struct). Re-run after changing the layout.

  python3 tools/gen_assets.py          # writes into ../assets

Outputs (all small, all committed):
  sprites/actors.png + actors.json    Aseprite-style sheet (array form, frameTags) with 8 two-frame
                                      tags: player grunt runner brute bullet heart rapid spread
  maps/tiles.png + arena.tsj          Tiled tileset (16 px)
  maps/arena.tmj                      Tiled map built from kArenaRows in src/shooter/arena.h (the rules'
                                      layout), so the drawn map always matches the TileWorld
  audio/*.wav                         short synthesized 16-bit mono effects (+ heartbeat.wav loop)
"""
import json
import math
import os
import random
import re
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
ASSETS = os.path.join(ROOT, "assets")
CELL = 16


# ---------------------------------------------------------------------------------------------
# PNG
def write_png(path, width, height, pixels):
    """pixels: list of rows, each a list of (r, g, b, a)."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)
        for (r, g, b, a) in row:
            raw += bytes((r, g, b, a))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[(0, 0, 0, 0) for _ in range(w)] for _ in range(h)]

    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c

    def rect(self, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.put(xx, yy, c)

    def disc(self, cx, cy, r, c):
        for yy in range(int(cy - r - 1), int(cy + r + 2)):
            for xx in range(int(cx - r - 1), int(cx + r + 2)):
                if (xx + 0.5 - cx) ** 2 + (yy + 0.5 - cy) ** 2 <= r * r:
                    self.put(xx, yy, c)

    def outline(self, c):
        """1-px dark outline around opaque pixels (readable on any floor)."""
        src = [row[:] for row in self.px]
        for y in range(self.h):
            for x in range(self.w):
                if src[y][x][3]:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < self.w and 0 <= yy < self.h and src[yy][xx][3] and src[yy][xx] != c:
                        self.px[y][x] = c
                        break


def blit(dst, src, ox, oy):
    for y in range(src.h):
        for x in range(src.w):
            if src.px[y][x][3]:
                dst.put(ox + x, oy + y, src.px[y][x])


OUT = (20, 16, 28, 255)


def actor(body, dark, eye, bob, wide=5.0, tall=5.5):
    """A round critter standing on the bottom edge (pivot = feet)."""
    c = Canvas(CELL, CELL)
    cy = 9.5 - bob
    c.disc(8, cy, wide if wide >= tall else tall, body)
    c.rect(4, int(cy) + 3, 8, 2, dark)
    c.rect(5, 14 - (1 if bob else 0), 2, 2, dark)
    c.rect(9, 14 - (0 if bob else 1), 2, 2, dark)
    c.put(6, int(cy) - 1, eye)
    c.put(10, int(cy) - 1, eye)
    c.outline(OUT)
    return c


def player(frame):
    c = Canvas(CELL, CELL)
    bob = frame
    c.disc(8, 10 - bob, 4.5, (70, 140, 230, 255))     # body
    c.disc(8, 5 - bob, 3.0, (240, 210, 170, 255))     # head
    c.rect(5, 3 - bob, 6, 1, (60, 60, 90, 255))        # hair
    c.rect(11, 9 - bob, 4, 2, (180, 180, 200, 255))    # gun
    c.rect(5, 14, 2, 2, (40, 60, 120, 255))
    c.rect(9, 14, 2, 2, (40, 60, 120, 255))
    c.outline(OUT)
    return c


def bullet(frame):
    c = Canvas(CELL, CELL)
    c.disc(8, 8, 2.6 if frame == 0 else 2.2, (255, 240, 140, 255))
    c.disc(8, 8, 1.2, (255, 255, 255, 255))
    return c


def heart(frame):
    c = Canvas(CELL, CELL)
    red = (235, 60, 80, 255)
    off = frame
    c.disc(6, 7 - off, 2.6, red)
    c.disc(10, 7 - off, 2.6, red)
    for i in range(5):
        c.rect(4 + i, 8 + i - off, 8 - 2 * i, 1, red)
    c.put(6, 6 - off, (255, 200, 210, 255))
    c.outline(OUT)
    return c


def orb(frame, color, glyph):
    c = Canvas(CELL, CELL)
    c.disc(8, 8 - frame, 5.5, color)
    c.disc(8, 8 - frame, 3.5, tuple(min(255, v + 60) for v in color[:3]) + (255,))
    for (x, y) in glyph:
        c.put(x, y - frame, (30, 30, 40, 255))
    c.outline(OUT)
    return c


R_GLYPH = [(7, 5), (8, 5), (7, 6), (9, 6), (7, 7), (8, 7), (7, 8), (9, 9), (7, 9), (7, 10), (9, 10)]
S_GLYPH = [(5, 9), (6, 9), (5, 10), (6, 10), (7, 5), (8, 5), (7, 6), (8, 6), (10, 9), (11, 9), (10, 10), (11, 10)]  # 3 dots

TAGS = [
    ("player", [player(0), player(1)]),
    ("grunt", [actor((90, 190, 90, 255), (40, 110, 50, 255), (255, 255, 255, 255), b) for b in (0, 1)]),
    ("runner", [actor((220, 80, 70, 255), (130, 30, 30, 255), (255, 230, 120, 255), b, wide=4.0, tall=4.5) for b in (0, 1)]),
    ("brute", [actor((150, 90, 200, 255), (80, 40, 120, 255), (255, 120, 120, 255), b, wide=6.0, tall=6.0) for b in (0, 1)]),
    ("bullet", [bullet(0), bullet(1)]),
    ("heart", [heart(0), heart(1)]),
    ("rapid", [orb(f, (240, 200, 60, 255), R_GLYPH) for f in (0, 1)]),
    ("spread", [orb(f, (80, 200, 230, 255), S_GLYPH) for f in (0, 1)]),
]


def write_sheet():
    frames = [f for _, fs in TAGS for f in fs]
    cols = 8
    rows = (len(frames) + cols - 1) // cols
    sheet = Canvas(cols * CELL, rows * CELL)
    meta_frames = []
    for i, f in enumerate(frames):
        x, y = (i % cols) * CELL, (i // cols) * CELL
        blit(sheet, f, x, y)
        meta_frames.append({
            "filename": "actors %d.aseprite" % i,
            "frame": {"x": x, "y": y, "w": CELL, "h": CELL},
            "rotated": False, "trimmed": False,
            "spriteSourceSize": {"x": 0, "y": 0, "w": CELL, "h": CELL},
            "sourceSize": {"w": CELL, "h": CELL},
            "duration": 120 if i // 2 == 4 else 220,
        })
    tags, at = [], 0
    for name, fs in TAGS:
        tags.append({"name": name, "from": at, "to": at + len(fs) - 1, "direction": "forward"})
        at += len(fs)
    write_png(os.path.join(ASSETS, "sprites", "actors.png"), sheet.w, sheet.h, sheet.px)
    doc = {"frames": meta_frames,
           "meta": {"app": "tools/gen_assets.py", "version": "1.3", "image": "actors.png",
                    "format": "RGBA8888", "size": {"w": sheet.w, "h": sheet.h}, "scale": "1",
                    "frameTags": tags, "layers": [{"name": "Layer", "opacity": 255, "blendMode": "normal"}],
                    "slices": []}}
    with open(os.path.join(ASSETS, "sprites", "actors.json"), "w") as f:
        json.dump(doc, f, indent=1)


# ---------------------------------------------------------------------------------------------
# Tiles and map
FLOOR = [(46, 44, 58), (50, 48, 62), (42, 40, 54)]
TILE_NAMES = ["floor0", "floor1", "floor2", "wall_top", "wall_face", "pad"]


def tile(kind):
    c = Canvas(CELL, CELL)
    rnd = random.Random(kind)
    if kind.startswith("floor"):
        base = FLOOR[int(kind[-1])]
        for y in range(CELL):
            for x in range(CELL):
                n = rnd.randint(-3, 3)
                c.put(x, y, (base[0] + n, base[1] + n, base[2] + n, 255))
        c.rect(0, 0, CELL, 1, (38, 36, 48, 255))
        c.rect(0, 0, 1, CELL, (38, 36, 48, 255))
        if kind == "floor2":
            for i in range(5):
                c.put(4 + i, 9 + (i % 2), (30, 28, 40, 255))
    elif kind == "wall_top":
        c.rect(0, 0, CELL, CELL, (92, 84, 110, 255))
        c.rect(0, 0, CELL, 2, (120, 112, 140, 255))
        for i in range(0, CELL, 4):
            c.put(rnd.randint(1, 14), i + 1, (80, 72, 96, 255))
    elif kind == "wall_face":
        c.rect(0, 0, CELL, CELL, (70, 62, 86, 255))
        c.rect(0, 0, CELL, 3, (120, 112, 140, 255))
        for y in (7, 12):
            c.rect(0, y, CELL, 1, (52, 46, 66, 255))
        for x in (5, 12):
            c.rect(x, 3, 1, 4, (52, 46, 66, 255))
        c.rect(8, 8, 1, 4, (52, 46, 66, 255))
    elif kind == "pad":
        c.rect(0, 0, CELL, CELL, (46, 44, 58, 255))
        c.disc(8, 8, 6.5, (120, 40, 60, 255))
        c.disc(8, 8, 4.5, (60, 24, 36, 255))
        c.disc(8, 8, 2, (180, 70, 90, 255))
    return c


def read_layout():
    text = open(os.path.join(ROOT, "src", "shooter", "arena.h")).read()
    body = text.split("ARENA-LAYOUT-BEGIN")[1].split("ARENA-LAYOUT-END")[0]
    rows = re.findall(r'"([#.SP]+)"', body)
    assert rows and all(len(r) == len(rows[0]) for r in rows), "ragged arena layout"
    return rows


def write_map():
    cols = 4
    sheet = Canvas(cols * CELL, 2 * CELL)
    for i, name in enumerate(TILE_NAMES):
        blit(sheet, tile(name), (i % cols) * CELL, (i // cols) * CELL)
    write_png(os.path.join(ASSETS, "maps", "tiles.png"), sheet.w, sheet.h, sheet.px)
    tsj = {"type": "tileset", "version": "1.10", "tiledversion": "1.10.2", "name": "arena",
           "image": "tiles.png", "imagewidth": sheet.w, "imageheight": sheet.h,
           "tilewidth": CELL, "tileheight": CELL, "tilecount": len(TILE_NAMES), "columns": cols,
           "margin": 0, "spacing": 0}
    with open(os.path.join(ASSETS, "maps", "arena.tsj"), "w") as f:
        json.dump(tsj, f, indent=1)

    rows = read_layout()
    h, w = len(rows), len(rows[0])
    gid = {n: i + 1 for i, n in enumerate(TILE_NAMES)}          # firstgid 1
    data = []
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch == "#":
                below = rows[y + 1][x] if y + 1 < h else "#"
                data.append(gid["wall_face"] if below != "#" else gid["wall_top"])
            elif ch == "S":
                data.append(gid["pad"])
            else:
                v = (x * 7349 + y * 1931 + x * y * 17) % 11
                data.append(gid["floor2"] if v == 0 else gid["floor1"] if v < 4 else gid["floor0"])
    tmj = {"type": "map", "version": "1.10", "tiledversion": "1.10.2", "orientation": "orthogonal",
           "renderorder": "right-down", "width": w, "height": h, "tilewidth": CELL, "tileheight": CELL,
           "infinite": False, "nextlayerid": 2, "nextobjectid": 1,
           "layers": [{"id": 1, "type": "tilelayer", "name": "ground", "x": 0, "y": 0, "width": w,
                       "height": h, "opacity": 1, "visible": True, "data": data}],
           "tilesets": [{"firstgid": 1, "source": "arena.tsj"}]}
    with open(os.path.join(ASSETS, "maps", "arena.tmj"), "w") as f:
        json.dump(tmj, f)


# ---------------------------------------------------------------------------------------------
# Audio
RATE = 22050


def write_wav(name, samples):
    path = os.path.join(ASSETS, "audio", name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    pcm = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 30000)) for s in samples)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
    hdr += b"data" + struct.pack("<I", len(pcm))
    with open(path, "wb") as f:
        f.write(hdr + pcm)


def tone(f0, f1, secs, wave="square", vol=0.5, noise=0.0, seed=1):
    rnd = random.Random(seed)
    n = int(RATE * secs)
    out, phase = [], 0.0
    for i in range(n):
        t = i / n
        f = f0 + (f1 - f0) * t
        phase += f / RATE
        if wave == "square":
            s = 1.0 if (phase % 1.0) < 0.5 else -1.0
        elif wave == "tri":
            s = 4 * abs((phase % 1.0) - 0.5) - 1
        else:
            s = math.sin(2 * math.pi * phase)
        s = s * (1 - noise) + (rnd.random() * 2 - 1) * noise
        env = min(1.0, i / 40.0) * (1 - t) ** 1.5
        out.append(s * env * vol)
    return out


def write_audio():
    write_wav("shoot.wav", tone(1400, 500, 0.07, "square", 0.25))
    write_wav("hit.wav", tone(500, 300, 0.05, "square", 0.3, noise=0.6, seed=2))
    write_wav("die1.wav", tone(300, 60, 0.22, "square", 0.4, noise=0.7, seed=3))
    write_wav("die2.wav", tone(360, 80, 0.2, "square", 0.4, noise=0.7, seed=4))
    write_wav("hurt.wav", tone(220, 90, 0.3, "square", 0.5, noise=0.3, seed=5))
    write_wav("pickup.wav", tone(600, 600, 0.06, "tri", 0.5) + tone(900, 900, 0.06, "tri", 0.5)
              + tone(1200, 1200, 0.1, "tri", 0.5))
    write_wav("wave.wav", tone(440, 440, 0.12, "tri", 0.5) + tone(660, 660, 0.2, "tri", 0.5))
    # Loops cleanly (low-HP alarm, played through a voice handle): lub-dub + rest, 0.8 s.
    beat = tone(90, 55, 0.09, "sine", 0.9) + [0.0] * int(RATE * 0.08) + tone(80, 50, 0.11, "sine", 0.8)
    write_wav("heartbeat.wav", beat + [0.0] * (int(RATE * 0.8) - len(beat)))
    write_wav("game_over.wav", tone(440, 420, 0.2, "tri", 0.5) + tone(330, 310, 0.2, "tri", 0.5)
              + tone(220, 200, 0.4, "tri", 0.5))


if __name__ == "__main__":
    write_sheet()
    write_map()
    write_audio()
    print("assets written to", ASSETS)
