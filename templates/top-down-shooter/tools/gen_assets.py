#!/usr/bin/env python3
"""Writes the template's placeholder assets (standard library only: zlib + struct).

    python3 tools/gen_assets.py        # from the game folder; writes into assets/

Re-run it after editing kArena in src/game/game.h, so the drawn map matches the walls. The outputs
are committed; replace them with real art when you have it (keep the file names, or update
assets/prefabs.json and src/main.cpp).

  sprites/actors.png + actors.json   Aseprite-style sheet (JSON array form, frameTags): two-frame
                                     tags "player", "enemy", "bullet" (16x16 cells, feet at bottom)
  maps/tiles.png + arena.tsj         Tiled tileset: floor, floor2, wall_top, wall_face, pad
  maps/arena.tmj                     Tiled map built from kArena (drawn only; rules own collision)
  audio/pop.wav                      a short 16-bit mono "pop" (enemy killed)
"""
import json
import os
import random
import re
import struct
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ASSETS = os.path.join(ROOT, "assets")
CELL = 16
OUTLINE = (20, 16, 28, 255)


def write_png(path, w, h, px):
    raw = b"".join(b"\x00" + bytes(c for p in row for c in p) for row in px)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[(0, 0, 0, 0)] * w for _ in range(h)]

    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c

    def rect(self, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.put(xx, yy, c)

    def disc(self, cx, cy, r, c):
        for yy in range(self.h):
            for xx in range(self.w):
                if (xx + 0.5 - cx) ** 2 + (yy + 0.5 - cy) ** 2 <= r * r:
                    self.put(xx, yy, c)

    def outline(self):
        src = [row[:] for row in self.px]
        for y in range(self.h):
            for x in range(self.w):
                if not src[y][x][3] and any(0 <= x + dx < self.w and 0 <= y + dy < self.h and src[y + dy][x + dx][3]
                                            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                    self.px[y][x] = OUTLINE
        return self

    def blit(self, src, ox, oy):
        for y in range(src.h):
            for x in range(src.w):
                if src.px[y][x][3]:
                    self.put(ox + x, oy + y, src.px[y][x])


# ---- Sprites: replace these little critters with your art (same tags, or edit prefabs.json) ----
def player(bob):
    c = Canvas(CELL, CELL)
    c.disc(8, 10 - bob, 4.5, (70, 140, 230, 255))      # body
    c.disc(8, 5 - bob, 3.0, (240, 210, 170, 255))      # head
    c.rect(11, 9 - bob, 4, 2, (190, 190, 210, 255))     # gun
    c.rect(5, 14, 2, 2, (40, 60, 120, 255))
    c.rect(9, 14, 2, 2, (40, 60, 120, 255))
    return c.outline()


def enemy(bob):
    c = Canvas(CELL, CELL)
    c.disc(8, 9.5 - bob, 5.5, (90, 190, 90, 255))
    c.rect(5, 14 - bob, 2, 2, (40, 110, 50, 255))
    c.rect(9, 13 + bob, 2, 2, (40, 110, 50, 255))
    c.put(6, int(8 - bob), (255, 255, 255, 255))
    c.put(10, int(8 - bob), (255, 255, 255, 255))
    return c.outline()


def bullet(frame):
    c = Canvas(CELL, CELL)
    c.disc(8, 8, 2.6 - 0.4 * frame, (255, 240, 140, 255))
    c.disc(8, 8, 1.2, (255, 255, 255, 255))
    return c


TAGS = [("player", [player(0), player(1)]), ("enemy", [enemy(0), enemy(1)]),
        ("bullet", [bullet(0), bullet(1)])]


def write_sheet():
    frames = [f for _, fs in TAGS for f in fs]
    sheet = Canvas(len(frames) * CELL, CELL)
    meta = []
    for i, f in enumerate(frames):
        sheet.blit(f, i * CELL, 0)
        meta.append({"filename": "actors %d.aseprite" % i, "frame": {"x": i * CELL, "y": 0, "w": CELL, "h": CELL},
                     "rotated": False, "trimmed": False,
                     "spriteSourceSize": {"x": 0, "y": 0, "w": CELL, "h": CELL},
                     "sourceSize": {"w": CELL, "h": CELL}, "duration": 200})
    tags, at = [], 0
    for name, fs in TAGS:
        tags.append({"name": name, "from": at, "to": at + len(fs) - 1, "direction": "forward"})
        at += len(fs)
    write_png(os.path.join(ASSETS, "sprites", "actors.png"), sheet.w, sheet.h, sheet.px)
    doc = {"frames": meta, "meta": {"app": "tools/gen_assets.py", "version": "1.3", "image": "actors.png",
                                    "format": "RGBA8888", "size": {"w": sheet.w, "h": sheet.h}, "scale": "1",
                                    "frameTags": tags, "slices": []}}
    with open(os.path.join(ASSETS, "sprites", "actors.json"), "w") as f:
        json.dump(doc, f, indent=1)


# ---- Tiles and the map --------------------------------------------------------------------------
TILES = ["floor", "floor2", "wall_top", "wall_face", "pad"]


def tile(kind):
    c = Canvas(CELL, CELL)
    rnd = random.Random(kind)
    if kind.startswith("floor"):
        base = (46, 44, 58) if kind == "floor" else (52, 50, 64)
        for y in range(CELL):
            for x in range(CELL):
                n = rnd.randint(-3, 3)
                c.put(x, y, (base[0] + n, base[1] + n, base[2] + n, 255))
        c.rect(0, 0, CELL, 1, (38, 36, 48, 255))
        c.rect(0, 0, 1, CELL, (38, 36, 48, 255))
    elif kind == "wall_top":
        c.rect(0, 0, CELL, CELL, (92, 84, 110, 255))
        c.rect(0, 0, CELL, 2, (120, 112, 140, 255))
    elif kind == "wall_face":
        c.rect(0, 0, CELL, CELL, (70, 62, 86, 255))
        c.rect(0, 0, CELL, 3, (120, 112, 140, 255))
        for y in (7, 12):
            c.rect(0, y, CELL, 1, (52, 46, 66, 255))
    elif kind == "pad":
        c.rect(0, 0, CELL, CELL, (46, 44, 58, 255))
        c.disc(8, 8, 6.5, (120, 40, 60, 255))
        c.disc(8, 8, 3, (60, 24, 36, 255))
    return c


def read_arena():
    text = open(os.path.join(ROOT, "src", "game", "game.h")).read()
    rows = re.findall(r'"([#.SP]+)"', text.split("ARENA-BEGIN")[1].split("ARENA-END")[0])
    assert rows and all(len(r) == len(rows[0]) for r in rows), "kArena rows must all have the same width"
    return rows


def write_map():
    sheet = Canvas(len(TILES) * CELL, CELL)
    for i, name in enumerate(TILES):
        sheet.blit(tile(name), i * CELL, 0)
    write_png(os.path.join(ASSETS, "maps", "tiles.png"), sheet.w, sheet.h, sheet.px)
    tsj = {"type": "tileset", "version": "1.10", "tiledversion": "1.10.2", "name": "arena", "image": "tiles.png",
           "imagewidth": sheet.w, "imageheight": sheet.h, "tilewidth": CELL, "tileheight": CELL,
           "tilecount": len(TILES), "columns": len(TILES), "margin": 0, "spacing": 0}
    with open(os.path.join(ASSETS, "maps", "arena.tsj"), "w") as f:
        json.dump(tsj, f, indent=1)
    rows = read_arena()
    h, w = len(rows), len(rows[0])
    gid = {n: i + 1 for i, n in enumerate(TILES)}               # firstgid 1
    data = []
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch == "#":
                below = rows[y + 1][x] if y + 1 < h else "#"
                data.append(gid["wall_face" if below != "#" else "wall_top"])
            else:
                data.append(gid["pad"] if ch == "S" else gid["floor2" if (x * 7 + y * 13) % 9 == 0 else "floor"])
    tmj = {"type": "map", "version": "1.10", "tiledversion": "1.10.2", "orientation": "orthogonal",
           "renderorder": "right-down", "width": w, "height": h, "tilewidth": CELL, "tileheight": CELL,
           "infinite": False, "nextlayerid": 2, "nextobjectid": 1,
           "layers": [{"id": 1, "type": "tilelayer", "name": "ground", "x": 0, "y": 0, "width": w, "height": h,
                       "opacity": 1, "visible": True, "data": data}],
           "tilesets": [{"firstgid": 1, "source": "arena.tsj"}]}
    with open(os.path.join(ASSETS, "maps", "arena.tmj"), "w") as f:
        json.dump(tmj, f)


# ---- Sound ----------------------------------------------------------------------------------------
def write_pop():
    rate, secs = 22050, 0.16
    rnd = random.Random(3)
    n = int(rate * secs)
    out, phase = [], 0.0
    for i in range(n):
        t = i / n
        phase += (320 - 260 * t) / rate                          # falling square wave + noise
        s = (1.0 if phase % 1.0 < 0.5 else -1.0) * 0.5 + (rnd.random() * 2 - 1) * 0.5
        out.append(s * min(1.0, i / 40.0) * (1 - t) ** 2 * 0.45)
    pcm = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, v)) * 30000)) for v in out)
    path = os.path.join(ASSETS, "audio", "pop.wav")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
                + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
                + b"data" + struct.pack("<I", len(pcm)) + pcm)


if __name__ == "__main__":
    write_sheet()
    write_map()
    write_pop()
    print("assets written to", ASSETS)
