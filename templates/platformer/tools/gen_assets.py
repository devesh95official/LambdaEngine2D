#!/usr/bin/env python3
"""Generates the template's placeholder assets (Python 3 standard library only).

    python3 tools/gen_assets.py

Writes, under assets/:
  sprites/tiles.png      4 tiles of 16 px: grass, dirt, spikes, plank
  sprites/actors.png     hero 16x16 at (0,0), coin 16x16 at (16,0), goal flag 16x32 at (32,0)
  maps/tiles.tsj         Tiled tileset: collision shapes and flags as tile properties
  maps/level1.tmj        Tiled map built from LEVEL below (a "ground" tile layer + point objects)
  sfx/coin.wav           the one sound
  sfx/game.sounds.json   sound bank: group "coin" (what the rules' PlaySound names)
prefabs.json is hand-written: edit it directly. The output is deterministic, so regenerating
without changes gives identical files. Replace any of these with real art; keep the names, or
update prefabs.json and the tileset.
"""
import json
import math
import os
import struct
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
TILE = 16

# TODO: edit the level here (or open maps/level1.tmj in Tiled and stop running this script).
# Each character is one 16 px cell. Map tiles: # ground  = one-way plank  ^ spikes
# Objects (prefab names in prefabs.json): @ hero  o coin  G goal.  Outside the map is solid, so
# a pit is a column with no ground; the rules respawn the hero when it falls below the last row.
LEVEL = [
    "........................................................",
    "........................................................",
    "........................................................",
    "........................................................",
    "........................................................",
    "........................................................",
    "........................................................",
    "........................................................",
    "........................................................",
    "........................................................",
    "........................................................",
    "........................................................",
    ".........oooo..............................oooo.........",
    ".........====....................#.........====.........",
    "..@..oo........#...oo.......^....#....ooo........^...G..",
    "######################..################################",
    "######################..################################",
]
OBJECTS = {"@": "hero", "o": "coin", "G": "goal"}
GRASS, DIRT, SPIKES, PLANK = range(4)                           # tile ids in tiles.png

# ---- tiny image writer --------------------------------------------------------------------------
def hexc(s):
    s = s.lstrip("#")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4)) + ((int(s[6:8], 16),) if len(s) == 8 else (255,))

class Image:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[(0, 0, 0, 0)] * w for _ in range(h)]

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c

    def rect(self, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.set(xx, yy, c)

    def save_png(self, path):
        raw = b"".join(b"\x00" + bytes(v for p in row for v in p) for row in self.px)
        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 6, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
        write(path, png)

def write(rel, data):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    print("wrote", os.path.relpath(path, os.path.join(ROOT, "..")))

def speckle(img, ox, y0, color, seed):
    for i in range(10):                                          # fixed pseudo-random dots
        x, y = (i * 7 + seed * 3) % 16, y0 + (i * 5 + seed) % (16 - y0)
        img.set(ox + x, y, color)

# ---- art ------------------------------------------------------------------------------------------
def tiles_png():
    img = Image(4 * TILE, TILE)
    brown, dark = hexc("#8b5a2b"), hexc("#6e4420")
    img.rect(GRASS * TILE, 0, TILE, TILE, brown)
    speckle(img, GRASS * TILE, 5, dark, 1)
    img.rect(GRASS * TILE, 0, TILE, 4, hexc("#5bbf4a"))
    img.rect(GRASS * TILE, 0, TILE, 1, hexc("#8ee07a"))
    img.rect(DIRT * TILE, 0, TILE, TILE, brown)
    speckle(img, DIRT * TILE, 0, dark, 4)
    for cx in (2, 7, 12):                                        # three grey spikes on the floor
        for row in range(8):
            half = row // 3
            img.rect(SPIKES * TILE + cx - half, 8 + row, half + 1, 1, hexc("#e4e4ec"))
            img.rect(SPIKES * TILE + cx + 1, 8 + row, half, 1, hexc("#9a9aa8"))
    img.rect(PLANK * TILE, 0, TILE, 5, hexc("#c68b4e"))
    img.rect(PLANK * TILE, 0, TILE, 1, hexc("#e0a96d"))
    img.rect(PLANK * TILE, 4, TILE, 1, hexc("#8a5a2e"))
    img.save_png("sprites/tiles.png")

def actors_png():
    img = Image(48, 32)
    img.rect(3, 3, 10, 13, hexc("#7a2333"))                      # hero: outline, body, eye, feet
    img.rect(4, 4, 8, 11, hexc("#e85d75"))
    img.rect(8, 6, 3, 3, hexc("#ffffff"))
    img.set(10, 7, hexc("#1b1b1b"))
    img.rect(4, 14, 3, 2, hexc("#3b3b58"))
    img.rect(9, 14, 3, 2, hexc("#3b3b58"))
    for y in range(16):                                          # coin: gold disc with a rim
        for x in range(16):
            d = math.hypot(x + 0.5 - 8, y + 0.5 - 8)
            if d <= 5.5:
                img.set(16 + x, y, hexc("#b8860b") if d > 4.5 else hexc("#ffd23f"))
    img.rect(16 + 6, 5, 2, 2, hexc("#fff3b0"))
    img.rect(32 + 3, 2, 2, 30, hexc("#dddddd"))                  # goal: pole and flag
    img.rect(32 + 2, 0, 4, 3, hexc("#ffd23f"))
    for i in range(9):
        img.rect(32 + 5, 3 + i, 9 - abs(4 - i) * 2, 1, hexc("#2ecc71"))
    img.save_png("sprites/actors.png")

# ---- sound ----------------------------------------------------------------------------------------
def coin_wav():
    rate, samples = 22050, []
    for freq, secs in ((988, 0.05), (1319, 0.10)):              # two rising notes
        n = int(rate * secs)
        for i in range(n):
            env = math.exp(-4.0 * i / n)
            v = math.sin(2 * math.pi * freq * i / rate) + 0.3 * math.sin(6 * math.pi * freq * i / rate)
            samples.append(int(max(-1.0, min(1.0, 0.45 * env * v)) * 32767))
    data = struct.pack("<%dh" % len(samples), *samples)
    header = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(data), b"WAVE", b"fmt ", 16, 1, 1, rate,
                         rate * 2, 2, 16, b"data", len(data))
    write("sfx/coin.wav", header + data)
    bank = {"bus": "sfx", "groups": {"coin": {"files": ["coin.wav"], "volume": 0.7, "pitch_jitter": 0.05}}}
    write("sfx/game.sounds.json", json.dumps(bank, indent=1) + "\n")

# ---- Tiled tileset and map ----------------------------------------------------------------------------
def tileset():
    def prop(name, kind, value):
        return {"name": name, "type": kind, "value": value}
    ts = {"type": "tileset", "version": "1.10", "tiledversion": "1.10.2", "name": "tiles",
          "image": "../sprites/tiles.png", "imagewidth": 4 * TILE, "imageheight": TILE,
          "tilewidth": TILE, "tileheight": TILE, "tilecount": 4, "columns": 4, "margin": 0, "spacing": 0,
          "tiles": [{"id": GRASS, "properties": [prop("shape", "string", "solid")]},
                    {"id": DIRT, "properties": [prop("shape", "string", "solid")]},
                    {"id": SPIKES, "properties": [prop("hazard", "bool", True)]},       # no shape: walk-in
                    {"id": PLANK, "properties": [prop("shape", "string", "one_way")]}]}
    write("maps/tiles.tsj", json.dumps(ts, indent=1) + "\n")

def level_map():
    h, w = len(LEVEL), len(LEVEL[0])
    assert all(len(row) == w for row in LEVEL), "every LEVEL row needs the same length"
    gids, objects = [], []
    for y, row in enumerate(LEVEL):
        for x, ch in enumerate(row):
            tile = {"#": GRASS if y == 0 or LEVEL[y - 1][x] != "#" else DIRT, "=": PLANK, "^": SPIKES}.get(ch)
            gids.append(0 if tile is None else tile + 1)          # gid = firstgid (1) + tile id
            if ch in OBJECTS:                                    # point objects: prefab position
                cx, bottom = x * TILE + TILE / 2, (y + 1) * TILE  # hero 1 px above the floor (feet),
                py = {"hero": bottom - 1, "goal": bottom}.get(OBJECTS[ch], y * TILE + TILE / 2)   # coins centred
                objects.append({"id": len(objects) + 1, "name": "", "type": OBJECTS[ch], "x": cx, "y": py,
                                "width": 0, "height": 0, "rotation": 0, "visible": True, "point": True})
    layers = [{"id": 1, "name": "ground", "type": "tilelayer", "x": 0, "y": 0, "width": w, "height": h,
               "opacity": 1, "visible": True, "data": "@DATA@"},
              {"id": 2, "name": "objects", "type": "objectgroup", "draworder": "topdown", "x": 0, "y": 0,
               "opacity": 1, "visible": True, "objects": objects}]
    m = {"type": "map", "version": "1.10", "tiledversion": "1.10.2", "orientation": "orthogonal",
         "renderorder": "right-down", "width": w, "height": h, "tilewidth": TILE, "tileheight": TILE,
         "infinite": False, "backgroundcolor": "#8ecae6", "nextlayerid": 3, "nextobjectid": len(objects) + 1,
         "tilesets": [{"firstgid": 1, "source": "tiles.tsj"}], "layers": layers}
    rows = ",\n  ".join(",".join(str(g) for g in gids[y * w:(y + 1) * w]) for y in range(h))
    write("maps/level1.tmj", json.dumps(m, indent=1).replace('"@DATA@"', "[\n  " + rows + "]") + "\n")
    print("\n".join(LEVEL))
    print("%dx%d tiles, %d coins" % (w, h, sum(row.count("o") for row in LEVEL)))

if __name__ == "__main__":
    tiles_png()
    actors_png()
    coin_wav()
    tileset()
    level_map()
