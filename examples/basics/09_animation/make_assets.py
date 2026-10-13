#!/usr/bin/env python3
"""Writes the animation_demo assets: a 9-frame 16x16 hero sheet (hero.png) with its Aseprite-style
JSON (hero.json: frame tags idle/run/attack and a "hitbox" slice on the attack frames), the
animation graph (hero.anim.json) and a one-frame coin (coin.png). Standard library only.
Run from anywhere: python3 examples/basics/09_animation/make_assets.py"""
import json
import os
import struct
import zlib

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "assets")
W = H = 16


def png(path, width, height, pixels):
    """pixels: list of rows, each a list of (r, g, b, a)."""
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(raw, 9))
    data += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


CLEAR = (0, 0, 0, 0)
SKIN = (240, 200, 160, 255)
SHIRT = (60, 110, 200, 255)
LEGS = (50, 50, 70, 255)
SWORD = (230, 230, 240, 255)


def hero_frame(leg_shift, arm, sword):
    img = [[CLEAR] * W for _ in range(H)]

    def rect(x0, y0, x1, y1, c):
        for y in range(max(y0, 0), min(y1, H)):
            for x in range(max(x0, 0), min(x1, W)):
                img[y][x] = c

    rect(6, 2, 10, 6, SKIN)                     # head
    rect(5, 6, 11, 11, SHIRT)                   # body
    rect(5 + leg_shift, 11, 7 + leg_shift, 16, LEGS)
    rect(9 - leg_shift, 11, 11 - leg_shift, 16, LEGS)
    rect(11, 7 + arm, 13, 9 + arm, SKIN)        # front arm
    if sword:
        rect(13, 7, 13 + sword, 9, SWORD)       # sword reaching right
    return img


# idle 0-1, run 2-5, attack 6-8
frames = [hero_frame(0, 0, 0), hero_frame(0, 1, 0),
          hero_frame(-1, 0, 0), hero_frame(0, 1, 0), hero_frame(1, 0, 0), hero_frame(0, 1, 0),
          hero_frame(0, -1, 0), hero_frame(0, 0, 3), hero_frame(0, 0, 1)]
durations = [300, 300, 90, 90, 90, 90, 80, 120, 100]
sheet = [[px for f in frames for px in f[y]] for y in range(H)]

os.makedirs(HERE, exist_ok=True)
png(os.path.join(HERE, "hero.png"), W * len(frames), H, sheet)
meta = {
    "app": "make_assets.py", "image": "hero.png", "format": "RGBA8888",
    "size": {"w": W * len(frames), "h": H}, "scale": "1",
    "frameTags": [
        {"name": "idle", "from": 0, "to": 1, "direction": "pingpong"},
        {"name": "run", "from": 2, "to": 5, "direction": "forward"},
        {"name": "attack", "from": 6, "to": 8, "direction": "forward", "repeat": "1"},
    ],
    "slices": [{"name": "hitbox", "color": "#ff0000ff", "keys": [
        {"frame": 6, "bounds": {"x": 0, "y": 0, "w": 0, "h": 0}},
        {"frame": 7, "bounds": {"x": 12, "y": 5, "w": 4, "h": 5}},
        {"frame": 8, "bounds": {"x": 0, "y": 0, "w": 0, "h": 0}}]}],
}
doc = {"frames": [{"filename": "hero %d.aseprite" % i,
                   "frame": {"x": i * W, "y": 0, "w": W, "h": H}, "rotated": False,
                   "trimmed": False, "spriteSourceSize": {"x": 0, "y": 0, "w": W, "h": H},
                   "sourceSize": {"w": W, "h": H}, "duration": d}
                  for i, d in enumerate(durations)], "meta": meta}
with open(os.path.join(HERE, "hero.json"), "w") as f:
    json.dump(doc, f, indent=1)
    f.write("\n")

graph = {
    "sheet": "hero.json",
    "start": "idle",
    "clips": [{"name": "idle", "tags": ["idle", "idle"]},
              {"name": "run", "tags": ["run", "run"]},
              {"name": "attack", "tags": ["attack", "attack"]}],
    "transitions": [
        {"from": "idle", "to": "run", "param": "moving", "above": 0.5},
        {"from": "run", "to": "idle", "param": "moving", "below": 0.5},
        {"from": "*", "to": "attack", "trigger": "attack"},
        {"from": "attack", "to": "idle", "on_finish": True},
    ],
    "events": [{"tag": "attack", "frame": 1, "name": "hit"}],
}
with open(os.path.join(HERE, "hero.anim.json"), "w") as f:
    json.dump(graph, f, indent=1)
    f.write("\n")

coin = [[CLEAR] * 8 for _ in range(8)]
for y in range(8):
    for x in range(8):
        if (x - 3.5) ** 2 + (y - 3.5) ** 2 <= 12.5:
            coin[y][x] = (250, 200, 40, 255)
png(os.path.join(HERE, "coin.png"), 8, 8, coin)
print("wrote", sorted(os.listdir(HERE)))
