#!/usr/bin/env python3
"""Prepares Pathar's assets in assets/ for the engine's loaders.

Run from anywhere: python3 tools/prepare_assets.py  (idempotent; standard library only)

1. sprites/props.json: adds a one-frame Aseprite frame tag per named frame ("stone", "stone_lit",
   "logo", ...). lam::SpriteSheet keeps tags but not frame names, so a tag is the only way to find
   a frame by name (SpriteSheet::find_tag).
2. tiles/pathar_tiles.tsj: a Tiled tileset for tiles/pathar_tiles.png (16x16 tiles, 16 columns),
   so the engine's .tsj loader can pack it into an atlas and the board can be drawn as a Tilemap.
   The game's own tiles/pathar_tiles.json (terrains: edges/base/details) is still read by the game
   to choose tiles (autotile rules).
3. Drops large audio the game does not use (chapter music loops, ambience, unused sfx).
"""
import json
import os
import struct

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    return struct.unpack(">II", head[16:24])


def tag_props():
    path = os.path.join(ROOT, "sprites", "props.json")
    with open(path) as f:
        sheet = json.load(f)
    frames = sheet["frames"]
    tags = [t for t in sheet["meta"].get("frameTags", []) if t["name"] == "diya_lit"]
    for i, fr in enumerate(frames):
        tags.append({"name": fr["filename"], "from": i, "to": i, "direction": "forward", "color": "#000000ff"})
    sheet["meta"]["frameTags"] = tags
    with open(path, "w") as f:
        json.dump(sheet, f, indent=1, sort_keys=True)
        f.write("\n")


def write_tsj():
    w, h = png_size(os.path.join(ROOT, "tiles", "pathar_tiles.png"))
    tsj = {
        "type": "tileset",
        "name": "pathar_tiles",
        "image": "pathar_tiles.png",
        "imagewidth": w,
        "imageheight": h,
        "tilewidth": 16,
        "tileheight": 16,
        "tilecount": (w // 16) * (h // 16),
        "columns": w // 16,
        "margin": 0,
        "spacing": 0,
    }
    with open(os.path.join(ROOT, "tiles", "pathar_tiles.tsj"), "w") as f:
        json.dump(tsj, f, indent=1, sort_keys=True)
        f.write("\n")


KEEP_SFX = {
    "bump_1", "footstep_grass_1", "footstep_grass_2", "footstep_grass_3", "footstep_stone_1",
    "footstep_stone_2", "footstep_stone_3", "gate_open_1", "item_gain_1", "lamp_wrong_1", "level_up_1",
    "puzzle_solved_1", "stone_push_1", "stone_push_2", "stone_push_3", "stone_set_1", "stone_splash_1",
    "stone_splash_2", "ui_back_1", "ui_confirm_1", "ui_error_1", "ui_move_1", "undo_1",
}
KEEP_MUSIC = {"pathar_title", "discover", "victory"}


def prune_audio():
    for folder, keep in (("sfx", KEEP_SFX), ("music", KEEP_MUSIC)):
        d = os.path.join(ROOT, "audio", folder)
        for name in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            if os.path.splitext(name)[0] not in keep:
                os.remove(os.path.join(d, name))
    amb = os.path.join(ROOT, "audio", "ambience")
    if os.path.isdir(amb):
        for name in os.listdir(amb):
            os.remove(os.path.join(amb, name))
        os.rmdir(amb)


if __name__ == "__main__":
    tag_props()
    write_tsj()
    prune_audio()
    print("pathar assets prepared")
