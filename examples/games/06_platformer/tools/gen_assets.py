#!/usr/bin/env python3
"""Generates every platformer asset (stdlib only: zlib + struct + json + wave math).

    python3 examples/games/06_platformer/tools/gen_assets.py

Writes into ../assets: pixel-art PNGs, Aseprite-style JSON sheets, an AnimGraph (.anim.json), a
Tiled tileset (.tsj) and level (.tmj), tiny WAV beeps and a sound bank. The level layout is built
in level_rows() and printed as ASCII (one character per 16x16 tile) when the script runs.
"""
import json
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "assets")
T = 16  # tile size

# ---------------------------------------------------------------------------------------------
# PNG writer (RGBA8, no filtering) and a tiny canvas.


def write_png(path, w, h, px):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw.extend(px[y * w + x])

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [(0, 0, 0, 0)] * (w * h)

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = c

    def rect(self, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.set(xx, yy, c)

    def save(self, name):
        write_png(os.path.join(OUT, name), self.w, self.h, self.px)


def rgb(h, a=255):
    return ((h >> 16) & 255, (h >> 8) & 255, h & 255, a)


GRASS, GRASS_HI, DIRT, DIRT_DK = rgb(0x4CAF50), rgb(0x8BD96B), rgb(0x8D5B3A), rgb(0x6B4226)
STONE, STONE_HI, STONE_DK = rgb(0x8A8F98), rgb(0xB5BAC2), rgb(0x5B6068)
WOOD, WOOD_DK, METAL, METAL_HI = rgb(0xC08A4A), rgb(0x7A5228), rgb(0x9AA4AE), rgb(0xE1E7EC)
SKIN, HAIR, SHIRT, PANTS, EYE = rgb(0xF2C79A), rgb(0xC0392B), rgb(0x2E86DE), rgb(0x34495E), rgb(0x111111)
SLIME, SLIME_HI, YELLOW, YELLOW_DK = rgb(0x6AC46A), rgb(0xB6F2A0), rgb(0xFFD54A), rgb(0xC9961A)
RED, WHITE, GREEN_FLAG = rgb(0xE74C3C), rgb(0xFFFFFF), rgb(0x2ECC71)

# ---------------------------------------------------------------------------------------------
# Tileset: 8 x 2 tiles. Local ids are what Tiled stores (gid = firstgid + local).
TILE_GRASS, TILE_DIRT, TILE_SLOPE_UR, TILE_SLOPE_UL, TILE_PLANK, TILE_LADDER, TILE_SPIKES, TILE_STONE = range(8)
TILE_BUSH, TILE_FLOWER, TILE_SIGN = 8, 9, 10


def specks(c, ox, oy, seed):
    for i in range(6):
        c.set(ox + (seed * 7 + i * 5) % 16, oy + 6 + (seed * 3 + i * 7) % 10, DIRT_DK)


def draw_tileset():
    c = Canvas(8 * T, 2 * T)

    def at(i):
        return (i % 8) * T, (i // 8) * T

    x, y = at(TILE_GRASS)
    c.rect(x, y, T, T, DIRT)
    specks(c, x, y, 1)
    c.rect(x, y, T, 4, GRASS)
    for i in range(0, T, 3):
        c.set(x + i, y, GRASS_HI)
        c.set(x + i + 1, y + 4, GRASS)
    x, y = at(TILE_DIRT)
    c.rect(x, y, T, T, DIRT)
    specks(c, x, y, 2)
    for i in range(T):  # slope up-right: floor rises to the right (filled below the diagonal)
        x, y = at(TILE_SLOPE_UR)
        top = T - 1 - i
        c.rect(x + i, y + top, 1, T - top, DIRT)
        c.rect(x + i, y + top, 1, 3, GRASS)
        x, y = at(TILE_SLOPE_UL)
        top = i
        c.rect(x + i, y + top, 1, T - top, DIRT)
        c.rect(x + i, y + top, 1, 3, GRASS)
    x, y = at(TILE_PLANK)
    c.rect(x, y, T, 5, WOOD)
    c.rect(x, y + 4, T, 1, WOOD_DK)
    c.rect(x + 2, y + 5, 2, 4, WOOD_DK)
    c.rect(x + 12, y + 5, 2, 4, WOOD_DK)
    x, y = at(TILE_LADDER)
    c.rect(x + 3, y, 2, T, WOOD_DK)
    c.rect(x + 11, y, 2, T, WOOD_DK)
    for r in range(1, T, 4):
        c.rect(x + 3, y + r, 10, 2, WOOD)
    x, y = at(TILE_SPIKES)
    for s in range(4):                       # four upward spikes on the bottom half
        for r in range(8):
            w = 1 + r // 2
            c.rect(x + s * 4 + (4 - w + 1) // 2, y + 8 + r, w, 1, METAL)
        c.set(x + s * 4 + 2, y + 8, METAL_HI)
    x, y = at(TILE_STONE)
    c.rect(x, y, T, T, STONE)
    c.rect(x, y, T, 1, STONE_HI)
    c.rect(x, y, 1, T, STONE_HI)
    c.rect(x, y + T - 1, T, 1, STONE_DK)
    c.rect(x + T - 1, y, 1, T, STONE_DK)
    x, y = at(TILE_BUSH)
    for yy in range(8, 16):
        for xx in range(16):
            if (xx - 8) ** 2 / 64 + (yy - 16) ** 2 / 64 <= 1:
                c.set(x + xx, y + yy, GRASS if (xx + yy) % 5 else GRASS_HI)
    x, y = at(TILE_FLOWER)
    c.rect(x + 7, y + 9, 1, 7, GRASS)
    c.rect(x + 6, y + 7, 3, 3, RED)
    c.set(x + 7, y + 8, YELLOW)
    x, y = at(TILE_SIGN)
    c.rect(x + 7, y + 8, 2, 8, WOOD_DK)
    c.rect(x + 2, y + 3, 12, 6, WOOD)
    c.rect(x + 9, y + 5, 3, 2, WOOD_DK)
    c.rect(x + 4, y + 5, 6, 2, WOOD_DK)
    c.save("sprites/tiles.png")


# ---------------------------------------------------------------------------------------------
# Characters and props. Each sheet is a horizontal strip plus an Aseprite "array" JSON.


def hero_frame(c, ox, pose, step):
    """pose: idle | run | jump | fall. Facing right; the engine mirrors 1-tag clips for left."""
    bob = 1 if (pose == "idle" and step == 1) else 0
    c.rect(ox + 5, 1 + bob, 7, 3, HAIR)              # cap
    c.rect(ox + 11, 3 + bob, 3, 1, HAIR)             # visor
    c.rect(ox + 5, 4 + bob, 7, 4, SKIN)              # face
    c.set(ox + 10, 5 + bob, EYE)
    c.rect(ox + 5, 8 + bob, 7, 4, SHIRT)             # body
    if pose == "jump":
        c.rect(ox + 3, 6, 2, 3, SKIN)
        c.rect(ox + 12, 6, 2, 3, SKIN)
        c.rect(ox + 5, 12, 3, 2, PANTS)
        c.rect(ox + 9, 12, 3, 3, PANTS)
    elif pose == "fall":
        c.rect(ox + 2, 8, 3, 2, SKIN)
        c.rect(ox + 12, 8, 3, 2, SKIN)
        c.rect(ox + 5, 12, 2, 4, PANTS)
        c.rect(ox + 10, 12, 2, 4, PANTS)
    else:
        swing = [0, 1, 0, -1][step % 4] if pose == "run" else 0
        c.rect(ox + 4 - swing, 9 + bob, 1, 3, SKIN)
        c.rect(ox + 12 + swing, 9 + bob, 1, 3, SKIN)
        if pose == "run":
            legs = [(5, 10), (6, 9), (7, 8), (6, 9)][step % 4]
            lift = [(0, 1), (0, 0), (1, 0), (0, 0)][step % 4]
            c.rect(ox + legs[0], 12, 2, 4 - lift[0], PANTS)
            c.rect(ox + legs[1], 12, 2, 4 - lift[1], PANTS)
        else:
            c.rect(ox + 6, 12 + bob, 2, 4 - bob, PANTS)
            c.rect(ox + 9, 12 + bob, 2, 4 - bob, PANTS)


def aseprite_json(image, fw, fh, n, durations, tags):
    frames = []
    for i in range(n):
        frames.append({
            "filename": f"{os.path.splitext(image)[0]} {i}.aseprite",
            "frame": {"x": i * fw, "y": 0, "w": fw, "h": fh},
            "rotated": False, "trimmed": False,
            "spriteSourceSize": {"x": 0, "y": 0, "w": fw, "h": fh},
            "sourceSize": {"w": fw, "h": fh},
            "duration": durations[i] if isinstance(durations, list) else durations,
        })
    return {"frames": frames, "meta": {
        "app": "gen_assets.py (Aseprite-compatible)", "version": "1.3", "image": image,
        "format": "RGBA8888", "size": {"w": fw * n, "h": fh}, "scale": "1",
        "frameTags": [{"name": t, "from": a, "to": b, "direction": d} for (t, a, b, d) in tags],
        "slices": []}}


def save_json(name, obj, compact=False):
    with open(os.path.join(OUT, name), "w") as f:
        if compact:
            json.dump(obj, f, separators=(",", ":"))
        else:
            json.dump(obj, f, indent=1)
        f.write("\n")


def draw_hero():
    c = Canvas(8 * T, T)
    poses = [("idle", 0), ("idle", 1), ("run", 0), ("run", 1), ("run", 2), ("run", 3), ("jump", 0), ("fall", 0)]
    for i, (p, s) in enumerate(poses):
        hero_frame(c, i * T, p, s)
    c.save("sprites/hero.png")
    save_json("sprites/hero.json", aseprite_json("hero.png", T, T, 8, [400, 400, 90, 90, 90, 90, 100, 100], [
        ("idle", 0, 1, "forward"), ("run", 2, 5, "forward"), ("jump", 6, 6, "forward"), ("fall", 7, 7, "forward")]))
    # AnimGraph (strict format, src/animation/README.md "Animation graphs"). A clip with no tag plays
    # the sheet tag of its own name; a 1-tag clip flips whenever the animator faces left (the graph's
    # facing space is at least 2). Paths inside an asset file are relative to that file: the sheet
    # is "hero.json", not "sprites/hero.json". Params come from MotorAnimBridge: speed (|vx|), vy,
    # grounded (0/1).
    clip = lambda n: {"name": n}
    above = lambda a, b, p, t: {"from": a, "to": b, "param": p, "above": t}
    below = lambda a, b, p, t: {"from": a, "to": b, "param": p, "below": t}
    save_json("sprites/hero.anim.json", {
        "sheet": "hero.json",
        "start": "idle",
        "clips": [clip("idle"), clip("run"), clip("jump"), clip("fall")],
        "transitions": [
            above("idle", "run", "speed", 12), below("run", "idle", "speed", 12),
            below("idle", "jump", "grounded", 0.5), below("run", "jump", "grounded", 0.5),
            above("jump", "fall", "vy", 0), above("fall", "idle", "grounded", 0.5),
            above("jump", "idle", "grounded", 0.5)]})


def draw_slime():
    c = Canvas(3 * T, T)
    for i, (h, w) in enumerate([(9, 12), (8, 14), (4, 15)]):
        ox, top = i * T, T - h
        for yy in range(h):
            half = w / 2 * math.sqrt(max(0.0, 1 - ((h - yy) / (h + 1)) ** 2)) + 1
            c.rect(ox + int(8 - half), top + yy, int(half * 2), 1, SLIME)
        c.rect(ox + 5, top + 2, 2, 1, SLIME_HI)
        if h > 5:
            c.set(ox + 6, top + 4, EYE)
            c.set(ox + 10, top + 4, EYE)
        else:
            c.rect(ox + 5, top + 2, 6, 1, EYE)
    c.save("sprites/slime.png")
    save_json("sprites/slime.json", aseprite_json("slime.png", T, T, 3, [220, 220, 120], [
        ("walk", 0, 1, "forward"), ("squash", 2, 2, "forward")]))


def draw_coin():
    c = Canvas(4 * T, T)
    for i, w in enumerate([10, 6, 2, 6]):
        ox = i * T
        for yy in range(3, 13):
            dy = (yy - 7.5) / 5
            half = w / 2 * math.sqrt(max(0.0, 1 - dy * dy))
            c.rect(ox + int(round(8 - half)), yy, max(1, int(round(half * 2))), 1, YELLOW)
        c.rect(ox + 7, 5, 1, 6, YELLOW_DK if w > 2 else YELLOW)
    c.save("sprites/coin.png")
    save_json("sprites/coin.json", aseprite_json("coin.png", T, T, 4, 120, [("spin", 0, 3, "forward")]))


def draw_flags():
    goal = Canvas(2 * T, 2 * T)
    for i in range(2):
        ox = i * T
        goal.rect(ox + 3, 2, 2, 30, METAL)
        goal.rect(ox + 2, 0, 4, 2, YELLOW)
        for xx in range(9):
            wave = int(round(math.sin((xx + i * 3) * 0.8)))
            goal.rect(ox + 5 + xx, 3 + wave, 1, 7, RED if (xx // 3 + i) % 2 else WHITE)
    goal.save("sprites/goal.png")
    save_json("sprites/goal.json", aseprite_json("goal.png", T, 2 * T, 2, 250, [("wave", 0, 1, "forward")]))
    cp = Canvas(3 * T, 2 * T)
    for i, col in enumerate([STONE, GREEN_FLAG, GRASS_HI]):
        ox = i * T
        cp.rect(ox + 7, 6, 2, 26, WOOD_DK)
        cp.rect(ox + 9, 6 + (0 if i else 12), 6, 5, col)
    cp.save("sprites/checkpoint.png")
    save_json("sprites/checkpoint.json", aseprite_json("checkpoint.png", T, 2 * T, 3, 200, [
        ("off", 0, 0, "forward"), ("on", 1, 2, "pingpong")]))


def draw_lift():
    c = Canvas(48, 8)
    c.rect(0, 0, 48, 8, METAL)
    c.rect(0, 0, 48, 1, METAL_HI)
    c.rect(0, 7, 48, 1, STONE_DK)
    for x in range(3, 48, 8):
        c.rect(x, 3, 2, 2, STONE_DK)
    c.save("sprites/lift.png")


def draw_crates():
    """Two stacked wooden crates (16 x 32): a Static entity solid, the step up to the coin plank."""
    c = Canvas(T, 2 * T)
    for oy in (0, T):
        c.rect(0, oy, T, T, WOOD_DK)
        c.rect(1, oy + 1, T - 2, T - 2, WOOD)
        for i in range(1, T - 1):                # diagonal brace
            c.set(i, oy + i, WOOD_DK)
            c.set(i + 1, oy + i, WOOD_DK)
        c.rect(1, oy + 1, T - 2, 1, WOOD_DK)
        c.rect(1, oy + T - 2, T - 2, 1, WOOD_DK)
    c.save("sprites/crates.png")


# ---------------------------------------------------------------------------------------------
# Level: 60 x 17 tiles (two 480 px screens). Legend:
#   # grass top  d dirt  / slope up-right  \ slope up-left  = one-way plank  H ladder
#   ^ spikes (hazard, no collision shape)  S stone  . empty
# Objects are placed by column/row in OBJECTS (prefab name = Tiled object type).
W, H = 60, 17


def level_rows():
    """The level as ASCII rows (printed by main() for reference). Legend:
    # grass top  d dirt  / slope up-right  \\ slope up-left  = one-way plank  H ladder
    ^ spikes (hazard flag, no collision shape)  S stone  . empty"""
    g = [["."] * W for _ in range(H)]

    def put(x0, x1, y, ch):                  # inclusive column range
        for x in range(x0, x1 + 1):
            g[y][x] = ch

    for y in range(H):
        g[y][0] = g[y][W - 1] = "S"          # side walls
    put(1, 58, 14, "#")                      # floor
    put(1, 58, 15, "d")
    put(1, 58, 16, "d")
    for x0, x1 in ((10, 11), (46, 48)):      # pits with spikes at the bottom
        put(x0, x1, 14, ".")
        put(x0, x1, 15, ".")
        put(x0, x1, 16, "^")
    put(13, 16, 10, "=")                     # one-way platform with coins
    g[13][18] = "/"                          # hill: slopes up, flat top, slopes down
    g[12][19] = "/"
    put(19, 22, 13, "d")
    put(20, 21, 12, "#")
    g[12][22] = "\\"
    g[13][23] = "\\"
    put(18, 23, 14, "d")
    put(26, 27, 13, "^")                     # spikes on the floor
    for x in (33, 43):                       # 2-tile walls fence the slime's patrol (cols 34-42)
        put(x, x, 12, "S")
        put(x, x, 13, "S")
    for y in range(8, 14):                   # ladder up to a one-way ledge
        g[y][52] = "H"
    put(53, 54, 8, "=")
    put(57, 58, 5, "=")                      # high ledge reached with the lift
    return ["".join(r) for r in g]


DECO = {(4, 13): TILE_BUSH, (14, 13): TILE_FLOWER, (25, 13): TILE_SIGN, (37, 13): TILE_BUSH,
        (45, 13): TILE_FLOWER, (50, 13): TILE_BUSH}

OBJECTS = [  # (prefab, column, row, anchor[, {float props}]) anchor: "feet" = 1 px above the tile below, "ground" = on it
            #   (Static bodies do not settle), "center", "lift" = platform centre
    ("hero", 3, 13, "feet"),
    ("coin", 5, 13, "center"), ("coin", 6, 13, "center"), ("coin", 7, 13, "center"),
    ("coin", 14, 9, "center"), ("coin", 15, 9, "center"), ("coin", 16, 9, "center"),
    ("coin", 21, 10, "center"),
    ("coin", 47, 11, "center"),
    ("coin", 53, 7, "center"), ("coin", 54, 7, "center"),
    ("coin", 57, 4, "center"), ("coin", 58, 4, "center"),
    ("checkpoint", 30, 13, "feet"),
    ("slime", 39, 13, "feet"),
    ("lift", 56, 11, "lift", {"travel_y": -80.0}),   # custom property -> PathFollower (lift_setup)
    ("goal", 58, 13, "feet"),
    ("crates", 16, 13, "ground"),          # under the plank's right end: 32 px up, then 32 px more
]


def build_level():
    rows = level_rows()
    for i, r in enumerate(rows):
        assert len(r) == W, f"row {i} has {len(r)} columns"
    legend = {"#": TILE_GRASS, "d": TILE_DIRT, "/": TILE_SLOPE_UR, "\\": TILE_SLOPE_UL, "=": TILE_PLANK,
              "H": TILE_LADDER, "^": TILE_SPIKES, "S": TILE_STONE}
    first_gid = 1
    ground = [0] * (W * H)
    for y, r in enumerate(rows):
        for x, ch in enumerate(r):
            if ch in legend:
                ground[y * W + x] = first_gid + legend[ch]
    deco = [0] * (W * H)
    for (x, y), t in DECO.items():
        deco[y * W + x] = first_gid + t
    objects = []
    prop = lambda n, t, v: {"name": n, "type": t, "value": v}
    for i, (kind, col, row, anchor, *extra) in enumerate(OBJECTS):
        px = col * T + T / 2
        if anchor == "feet":
            py = (row + 1) * T - 1          # 1 px above the floor; the motor settles it
        elif anchor == "ground":
            py = (row + 1) * T              # exactly on the floor (Static: nothing moves it)
        elif anchor == "lift":
            py = row * T + 4                # platform centre: top edge on the row line
        else:
            py = row * T + T / 2
        obj = {"id": i + 1, "name": kind, "type": kind, "x": px, "y": py, "width": 0, "height": 0,
               "point": True, "rotation": 0, "visible": True}
        if extra:
            obj["properties"] = [prop(k, "float", v) for k, v in extra[0].items()]
        objects.append(obj)
    tmj = {
        "type": "map", "version": "1.10", "tiledversion": "1.10.2", "orientation": "orthogonal",
        "renderorder": "right-down", "width": W, "height": H, "tilewidth": T, "tileheight": T,
        "infinite": False, "backgroundcolor": "#8ecae6", "nextlayerid": 4, "nextobjectid": len(objects) + 1,
        "properties": [prop("title", "string", "Hop Hill")],
        "tilesets": [{"firstgid": first_gid, "source": "tiles.tsj"}],
        "layers": [
            {"id": 1, "name": "deco", "type": "tilelayer", "width": W, "height": H, "x": 0, "y": 0,
             "opacity": 1, "visible": True, "data": deco,
             "properties": [prop("collision", "bool", False)]},   # drawn only; not in TileWorld
            {"id": 2, "name": "ground", "type": "tilelayer", "width": W, "height": H, "x": 0, "y": 0,
             "opacity": 1, "visible": True, "data": ground,
             "properties": [prop("collision", "bool", True)]},
            {"id": 3, "name": "objects", "type": "objectgroup", "draworder": "topdown", "x": 0, "y": 0,
             "opacity": 1, "visible": True, "objects": objects},
        ],
    }
    save_json("maps/level1.tmj", tmj, compact=True)
    # Tile properties: "shape" (solid | one_way | slope_up_right | slope_up_left) plus bool flags
    # "hazard" / "ladder"; the tile "type" becomes TileProps::kind.
    tiles = [
        (TILE_GRASS, "ground", [prop("shape", "string", "solid")]),
        (TILE_DIRT, "ground", [prop("shape", "string", "solid")]),
        (TILE_SLOPE_UR, "slope", [prop("shape", "string", "slope_up_right")]),
        (TILE_SLOPE_UL, "slope", [prop("shape", "string", "slope_up_left")]),
        (TILE_PLANK, "plank", [prop("shape", "string", "one_way")]),
        (TILE_LADDER, "ladder", [prop("ladder", "bool", True)]),
        (TILE_SPIKES, "spikes", [prop("hazard", "bool", True)]),
        (TILE_STONE, "stone", [prop("shape", "string", "solid")]),
    ]
    tsj = {"type": "tileset", "version": "1.10", "tiledversion": "1.10.2", "name": "tiles",
           "image": "../sprites/tiles.png", "imagewidth": 8 * T, "imageheight": 2 * T,
           "tilewidth": T, "tileheight": T, "tilecount": 16, "columns": 8, "margin": 0, "spacing": 0,
           "tiles": [{"id": i, "type": k, "properties": p} for (i, k, p) in tiles]}
    save_json("maps/tiles.tsj", tsj)
    return rows


# ---------------------------------------------------------------------------------------------
# Sounds: 8-bit mono 22050 Hz square/noise blips, a few KB each.


def write_wav(name, samples, rate=22050):
    data = bytes(max(0, min(255, int(128 + s * 100))) for s in samples)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate, 1, 8)
    hdr += b"data" + struct.pack("<I", len(data))
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(hdr + data)


def tone(freqs, seconds, rate=22050, noise=False):
    out, n, seed = [], int(seconds * rate), 12345
    for i in range(n):
        t = i / rate
        f = freqs[min(len(freqs) - 1, int(i * len(freqs) / n))]
        env = (1 - i / n) ** 1.5
        if noise:
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            v = (seed / 0x7FFFFFFF) * 2 - 1
        else:
            v = 1.0 if math.sin(2 * math.pi * f * t) >= 0 else -1.0
        out.append(v * env * 0.5)
    return out


def build_sounds():
    write_wav("sfx/coin.wav", tone([988, 1319], 0.12))
    write_wav("sfx/jump.wav", tone([330, 440, 554], 0.10))
    write_wav("sfx/stomp.wav", tone([220, 110], 0.12))
    write_wav("sfx/hurt.wav", tone([0], 0.25, noise=True))
    write_wav("sfx/checkpoint.wav", tone([523, 659, 784], 0.25))
    write_wav("sfx/win.wav", tone([523, 659, 784, 1047], 0.6))
    # Sound bank (strict format, src/audio/README.md "Sound banks"): "groups" is an object keyed by
    # group id, and file paths are relative to the bank file (so "coin.wav", not "sfx/coin.wav").
    groups = {n: {"files": [f"{n}.wav"], "volume": v, "max_voices": 2}
              for n, v in [("coin", 0.6), ("jump", 0.4), ("stomp", 0.7), ("hurt", 0.6),
                           ("checkpoint", 0.6), ("win", 0.8)]}
    save_json("sfx/platformer.sounds.json", {"bus": "sfx", "groups": groups})


def main():
    for d in ("maps", "sprites", "sfx"):
        os.makedirs(os.path.join(OUT, d), exist_ok=True)
    draw_tileset()
    draw_hero()
    draw_slime()
    draw_coin()
    draw_flags()
    draw_lift()
    draw_crates()
    rows = build_level()
    build_sounds()
    print("\n".join(rows))
    print("assets written to", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
