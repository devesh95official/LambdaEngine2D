#!/usr/bin/env python3
"""Generates every roguelike asset that is not hand-written JSON (stdlib only: zlib + struct).

    python3 tools/gen_assets.py     # from examples/games/02_roguelike; rewrites assets/ in place

Outputs (all small, all committed so the build never runs Python):
  assets/maps/tiles.png        8 map tiles, 16x16 (wall, floors, stairs, ...), next to its tileset
  assets/maps/dungeon.tsj      Tiled tileset over tiles.png with per-tile properties
  assets/maps/floor.tmj        Tiled map template: 64x40, every cell a wall (the sim carves it)
  assets/sprites/actors.png    16x16 glyph sheet, 2 frames per glyph (idle flicker)
  assets/sprites/actors.json   Aseprite-style sheet (array form) with one frame tag per glyph
  assets/sfx/*.wav             tiny synthesized effects (16-bit mono, 22050 Hz)
  assets/sfx/roguelike.sounds.json   the SoundBank naming them (groups the rules emit)
"""
import json
import math
import os
import random
import struct
import wave
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
T = 16  # tile and glyph size in pixels

# 5x7 bitmaps for the glyphs we draw (rows top to bottom, '#' = ink).
FONT = {
    "@": [".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".####"],
    "r": [".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."],
    "g": [".....", ".####", "#...#", "#...#", ".####", "....#", ".###."],
    "o": [".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "!": ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    "?": [".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."],
    "%": ["##..#", "##.#.", "..#..", ".#...", "#..##", "...##", "....."],
    ">": ["#....", ".#...", "..#..", "...#.", "..#..", ".#...", "#...."],
    "*": [".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."],
}


def png(path, width, height, pixels):
    """Writes RGBA8 `pixels` (list of rows of (r, g, b, a)) as a PNG."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)  # filter: none
        for (r, g, b, a) in row:
            raw += bytes((r, g, b, a))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    data += chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def canvas(w, h, fill=(0, 0, 0, 0)):
    return [[fill for _ in range(w)] for _ in range(h)]


def put(img, x, y, c):
    if 0 <= y < len(img) and 0 <= x < len(img[0]):
        img[y][x] = c


def glyph(img, ox, oy, ch, color, dy=0, shadow=(0, 0, 0, 255)):
    """Draws a 5x7 glyph at 2x scale (10x14) centred in the 16x16 cell, with a 1px dark outline."""
    bits = FONT[ch]
    x0, y0 = ox + 3, oy + 1 + dy
    for pass_ in (0, 1):
        for gy, row in enumerate(bits):
            for gx, cell in enumerate(row):
                if cell != "#":
                    continue
                for sy in range(2):
                    for sx in range(2):
                        px, py = x0 + gx * 2 + sx, y0 + gy * 2 + sy
                        if pass_ == 0:
                            for ddx, ddy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                                put(img, px + ddx, py + ddy, shadow)
                        else:
                            put(img, px, py, color)


def shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c[:3]) + (c[3],)


def make_tiles():
    """8 tiles in one row: 0 wall, 1 floor, 2 floor (pebbles), 3 stairs, 4 wall (cracked), 5..7 spare."""
    img = canvas(8 * T, T)
    stone, mortar = (92, 88, 104, 255), (52, 48, 62, 255)
    floor, dot = (36, 32, 40, 255), (70, 64, 76, 255)
    for t in range(8):
        ox = t * T
        for y in range(T):
            for x in range(T):
                if t in (0, 4):                                  # brick wall
                    row = y // 4
                    off = 4 if row % 2 else 0
                    edge = (y % 4 == 3) or ((x + off) % 8 == 7)
                    c = mortar if edge else shade(stone, 1.0 + 0.06 * ((x * 7 + y * 3) % 3 - 1))
                    if t == 4 and (x - y) in (2, 3) and 3 < y < 13:
                        c = mortar                               # crack
                    put(img, ox + x, y, c)
                elif t in (1, 2, 3):                             # floor
                    put(img, ox + x, y, floor)
                else:
                    put(img, ox + x, y, (0, 0, 0, 0))
        if t == 1:
            for (x, y) in ((4, 4), (11, 9)):
                put(img, ox + x, y, dot)
        if t == 2:
            for (x, y) in ((3, 11), (8, 5), (12, 12), (6, 8)):
                put(img, ox + x, y, dot)
        if t == 3:
            glyph(img, ox, 0, ">", (250, 220, 120, 255))
    png(os.path.join(ROOT, "maps", "tiles.png"), 8 * T, T, img)


GLYPHS = [  # (tag, char, colour)
    ("player", "@", (255, 236, 140, 255)),
    ("rat", "r", (186, 140, 100, 255)),
    ("goblin", "g", (120, 210, 90, 255)),
    ("orc", "o", (90, 160, 70, 255)),
    ("troll", "T", (180, 140, 220, 255)),
    ("potion", "!", (240, 80, 90, 255)),
    ("scroll", "?", (230, 210, 160, 255)),
    ("corpse", "%", (140, 40, 40, 255)),
    ("spark", "*", (160, 220, 255, 255)),
]


def make_actors():
    """Two frames per glyph: frame 0 normal, frame 1 one pixel higher and brighter (idle bob)."""
    cols = 16
    count = len(GLYPHS) * 2
    rows = (count + cols - 1) // cols
    img = canvas(cols * T, rows * T)
    frames, tags = [], []
    for i, (tag, ch, color) in enumerate(GLYPHS):
        for f in range(2):
            n = i * 2 + f
            ox, oy = (n % cols) * T, (n // cols) * T
            glyph(img, ox, oy, ch, color if f == 0 else shade(color, 1.15), dy=-1 if f else 0)
            frames.append({
                "filename": f"{tag} {f}",
                "frame": {"x": ox, "y": oy, "w": T, "h": T},
                "rotated": False, "trimmed": False,
                "spriteSourceSize": {"x": 0, "y": 0, "w": T, "h": T},
                "sourceSize": {"w": T, "h": T},
                "duration": 450 if f == 0 else 350,
            })
        tags.append({"name": tag, "from": i * 2, "to": i * 2 + 1, "direction": "forward"})
    png(os.path.join(ROOT, "sprites", "actors.png"), cols * T, rows * T, img)
    sheet = {"frames": frames, "meta": {
        "app": "tools/gen_assets.py", "version": "1", "image": "actors.png", "format": "RGBA8888",
        "size": {"w": cols * T, "h": rows * T}, "scale": "1", "frameTags": tags, "slices": []}}
    with open(os.path.join(ROOT, "sprites", "actors.json"), "w") as f:
        json.dump(sheet, f, indent=1)


def prop(name, kind, value):
    return {"name": name, "type": kind, "value": value}


def make_tileset_and_map():
    # Per-tile properties map onto lam::TileProps (shape, flags, cost, kind). Walls are solid,
    # opaque (FOV) and impassable (cost 0); floors and stairs are walkable.
    wall = [prop("shape", "string", "solid"), prop("opaque", "bool", True), prop("cost", "int", 0)]
    tiles = [
        {"id": 0, "type": "wall", "properties": wall},
        {"id": 1, "type": "floor", "properties": [prop("cost", "int", 10)]},
        {"id": 2, "type": "floor", "properties": [prop("cost", "int", 10)]},
        {"id": 3, "type": "stairs", "properties": [prop("cost", "int", 10)]},
        {"id": 4, "type": "wall", "properties": wall},
    ]
    tsj = {"type": "tileset", "version": "1.10", "tiledversion": "1.10.2", "name": "dungeon",
           "tilewidth": T, "tileheight": T, "tilecount": 8, "columns": 8, "margin": 0, "spacing": 0,
           "image": "tiles.png", "imagewidth": 8 * T, "imageheight": T, "tiles": tiles}
    os.makedirs(os.path.join(ROOT, "maps"), exist_ok=True)
    with open(os.path.join(ROOT, "maps", "dungeon.tsj"), "w") as f:
        json.dump(tsj, f, indent=1)
    w, h = 64, 40
    tmj = {"type": "map", "version": "1.10", "tiledversion": "1.10.2", "orientation": "orthogonal",
           "renderorder": "right-down", "infinite": False, "width": w, "height": h,
           "tilewidth": T, "tileheight": T, "nextlayerid": 2, "nextobjectid": 1,
           "tilesets": [{"firstgid": 1, "source": "dungeon.tsj"}],
           "layers": [{"id": 1, "name": "dungeon", "type": "tilelayer", "x": 0, "y": 0,
                       "width": w, "height": h, "opacity": 1, "visible": True,
                       "properties": [prop("collision", "bool", True)],
                       "data": [1] * (w * h)}]}
    with open(os.path.join(ROOT, "maps", "floor.tmj"), "w") as f:
        json.dump(tmj, f, separators=(",", ":"))


RATE = 22050


def wav(path, samples):
    """Writes float samples in [-1, 1] as 16-bit mono PCM."""
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, v)) * 30000))
                               for v in samples))


def tone(seconds, f0, f1, wave_kind="square", noise=0.0, volume=0.5, seed=1):
    """A pitch sweep f0 -> f1 Hz with a fast attack and linear decay, optionally noisy."""
    rng = random.Random(seed)
    n = int(seconds * RATE)
    out, phase = [], 0.0
    for i in range(n):
        t = i / n
        phase += (f0 + (f1 - f0) * t) / RATE
        if wave_kind == "square":
            v = 1.0 if (phase % 1.0) < 0.5 else -1.0
        elif wave_kind == "tri":
            v = 4.0 * abs((phase % 1.0) - 0.5) - 1.0
        else:
            v = math.sin(2 * math.pi * phase)
        v = v * (1.0 - noise) + (rng.random() * 2.0 - 1.0) * noise
        env = min(1.0, i / (0.004 * RATE)) * (1.0 - t)
        out.append(v * env * volume)
    return out


def make_sounds():
    d = os.path.join(ROOT, "sfx")
    os.makedirs(d, exist_ok=True)
    wav(os.path.join(d, "hit_1.wav"), tone(0.07, 420, 160, "square", 0.55, 0.45, seed=1))
    wav(os.path.join(d, "hit_2.wav"), tone(0.07, 380, 140, "square", 0.6, 0.45, seed=2))
    wav(os.path.join(d, "kill.wav"), tone(0.16, 520, 90, "square", 0.35, 0.45, seed=3))
    wav(os.path.join(d, "death.wav"), tone(0.7, 300, 45, "tri", 0.15, 0.6, seed=4))
    wav(os.path.join(d, "stairs.wav"), tone(0.12, 330, 330, "tri", 0.0, 0.4)
        + tone(0.12, 247, 247, "tri", 0.0, 0.4) + tone(0.2, 165, 165, "tri", 0.0, 0.4))
    wav(os.path.join(d, "pickup.wav"), tone(0.06, 660, 990, "sine", 0.0, 0.35)
        + tone(0.08, 990, 1320, "sine", 0.0, 0.35))
    bank = {"bus": "sfx", "groups": {
        "hit": {"file": "hit.wav", "variants": 2, "pitch_jitter": 0.08, "max_voices": 4},
        "kill": {"file": "kill.wav", "pitch_jitter": 0.05},
        "death": {"file": "death.wav", "pitch_jitter": 0, "max_voices": 1},
        "stairs": {"file": "stairs.wav", "pitch_jitter": 0, "max_voices": 1},
        "pickup": {"file": "pickup.wav", "pitch_jitter": 0.04, "volume": 0.8},
    }}
    with open(os.path.join(d, "roguelike.sounds.json"), "w") as f:
        json.dump(bank, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    make_tiles()
    make_actors()
    make_tileset_and_map()
    make_sounds()
    print("assets written to", os.path.normpath(ROOT))
