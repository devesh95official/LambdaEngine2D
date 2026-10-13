#!/usr/bin/env python3
"""Writes assets/levels/orchard.map.json (a native LambdaEngine2D map, schema in
src/tilemap/include/lam/tilemap/tilemap.h) from the text board assets/levels/orchard.txt
('#' wall, '.' grass, 'P' start). Standard library only.

The rows are the text board unchanged: the legend maps every character to a terrain, and 'P'
is grass with a 1x1 "start" object on its cell (legend {"terrain", "object"}), so the start needs
no separate coordinates. Run from anywhere:  python3 tools/make_map.py
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
LEVELS = os.path.join(HERE, "..", "assets", "levels")

rows = [line.rstrip("\r\n") for line in open(os.path.join(LEVELS, "orchard.txt"))]
rows = [r for r in rows if r]

doc = {
    "tile_size": 16,
    "terrains": {
        "wall": {"shape": "solid", "flags": ["opaque"], "cost": 0},
        "grass": {},
    },
    "legend": {"#": "wall", ".": "grass", "P": {"terrain": "grass", "object": "start"}},
    "rows": rows,
}
with open(os.path.join(LEVELS, "orchard.map.json"), "w") as f:
    json.dump(doc, f, indent=1)
    f.write("\n")
print("wrote orchard.map.json", len(rows[0]), "x", len(rows))
