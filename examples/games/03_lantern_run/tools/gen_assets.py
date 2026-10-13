#!/usr/bin/env python3
"""Generates Lantern Run's derived assets (standard library only: zlib/struct for PNG, json).

  assets/maps/ground.png   8 ground tiles (16x16): three grasses, speck, flower, two forest edges
  assets/maps/ground.tsj   Tiled tileset for it
  assets/maps/forest.tmj   Tiled map with one "ground" layer made from assets/levels/forest.txt,
                           with a per-cell hash scattering the grass variants
  assets/sprites/world.json  the world sheet plus one single-frame tag per static frame used by
                           prefabs (tree_saal_base, tree_saal_top, rock_small, diya_glow, shadow):
                           SpriteSheet keeps tags, not frame names, so an Animator needs a tag.

Run from anywhere: python3 tools/gen_assets.py  (idempotent; keep the outputs in git).
"""
import json
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
TILE = 16


def write_png(path, width, height, pixels):
    """pixels: list of rows, each a list of (r, g, b, a)."""
    raw = bytearray()
    for row in pixels:
        raw.append(0)
        for r, g, b, a in row:
            raw += bytes((r, g, b, a))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def tile_image():
    cols, rows = 4, 2
    img = [[(0, 0, 0, 0)] * (cols * TILE) for _ in range(rows * TILE)]

    def fill(tx, ty, colour):
        for y in range(TILE):
            for x in range(TILE):
                img[ty * TILE + y][tx * TILE + x] = colour

    def rect(tx, ty, x0, y0, w, h, colour):
        for y in range(y0, min(TILE, y0 + h)):
            for x in range(x0, min(TILE, x0 + w)):
                img[ty * TILE + y][tx * TILE + x] = colour

    def circle(tx, ty, cx, cy, r, colour):
        for y in range(TILE):
            for x in range(TILE):
                if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= r * r:
                    img[ty * TILE + y][tx * TILE + x] = colour

    grass_a, grass_b, grass_c = (74, 114, 54, 255), (70, 110, 52, 255), (78, 118, 56, 255)
    fill(0, 0, grass_a)                                  # 1
    fill(1, 0, grass_b)                                  # 2
    fill(2, 0, grass_c)                                  # 3
    fill(3, 0, grass_a)                                  # 4: grass with a blade
    rect(3, 0, 5, 7, 1, 2, (104, 150, 70, 255))
    fill(0, 1, grass_a)                                  # 5: grass with a flower
    rect(0, 1, 6, 7, 2, 2, (230, 210, 120, 255))
    for i, tx in enumerate((1, 2)):                      # 6, 7: forest edge (two variants)
        fill(tx, 1, (30, 58, 36, 255))
        circle(tx, 1, 5 + 3 * i, 6 + i, 5.0, (44, 78, 46, 255))
        circle(tx, 1, 11, 11, 4.0, (38, 70, 42, 255))
    fill(3, 1, (92, 78, 52, 255))                        # 8: spare (dirt)
    return cols * TILE, rows * TILE, img


def ground_gid(x, y, c):
    if c == "#":
        return 6 + ((x * 3) % 5) % 2
    h = ((x * 73856093) ^ (y * 19349663)) & 0xFFFFFFFF   # per-cell hash for the grass variants
    if h % 23 == 0:
        return 5
    if h % 5 == 0:
        return 4
    if h % 7 == 0:
        return 3
    if h % 3 == 0:
        return 2
    return 1


def make_ground():
    maps = os.path.join(ASSETS, "maps")
    os.makedirs(maps, exist_ok=True)
    w, h, img = tile_image()
    write_png(os.path.join(maps, "ground.png"), w, h, img)
    tileset = {"type": "tileset", "version": "1.10", "tiledversion": "1.10.2", "name": "ground",
               "tilewidth": TILE, "tileheight": TILE, "tilecount": 8, "columns": 4,
               "image": "ground.png", "imagewidth": w, "imageheight": h, "margin": 0, "spacing": 0}
    with open(os.path.join(maps, "ground.tsj"), "w") as f:
        json.dump(tileset, f, indent=1)
        f.write("\n")

    with open(os.path.join(ASSETS, "levels", "forest.txt")) as f:
        rows = [r.rstrip("\r\n") for r in f if r.strip()]
    width, height = max(len(r) for r in rows), len(rows)
    data = []
    for y in range(height):
        for x in range(width):
            c = rows[y][x] if x < len(rows[y]) else "#"
            data.append(ground_gid(x, y, c))
    tmj = {"type": "map", "version": "1.10", "tiledversion": "1.10.2", "orientation": "orthogonal",
           "renderorder": "right-down", "infinite": False, "width": width, "height": height,
           "tilewidth": TILE, "tileheight": TILE, "nextlayerid": 2, "nextobjectid": 1,
           "compressionlevel": -1,
           "layers": [{"id": 1, "name": "ground", "type": "tilelayer", "x": 0, "y": 0,
                       "width": width, "height": height, "opacity": 1, "visible": True,
                       "data": data}],
           "tilesets": [{"firstgid": 1, "source": "ground.tsj"}]}
    with open(os.path.join(maps, "forest.tmj"), "w") as f:
        json.dump(tmj, f, separators=(",", ":"))
        f.write("\n")


STATIC_FRAMES = ("tree_saal_base", "tree_saal_top", "rock_small", "diya_glow", "shadow")


def tag_static_frames():
    path = os.path.join(ASSETS, "sprites", "world.json")
    with open(path) as f:
        sheet = json.load(f)
    names = [fr["filename"] for fr in sheet["frames"]]
    tags = sheet["meta"].setdefault("frameTags", [])
    have = {t["name"] for t in tags}
    for name in STATIC_FRAMES:
        if name in have:
            continue
        i = names.index(name)
        tags.append({"name": name, "from": i, "to": i, "direction": "forward"})
    with open(path, "w") as f:
        json.dump(sheet, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    make_ground()
    tag_static_frames()
    print("lantern_run assets generated")
