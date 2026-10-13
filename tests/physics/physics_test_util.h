#pragma once
// Helpers shared by the lam_physics tests: ASCII tile maps and feet-anchored boxes.
#include <cstddef>
#include <initializer_list>
#include <string_view>

#include <doctest/doctest.h>
#include <lam/math/rect.h>
#include <lam/physics/tile_world.h>

namespace lam::test {

// One character per tile, rows top to bottom, all rows the same length:
//   '#' Solid, '=' OneWay, '/' SlopeUpRight, '\\' SlopeUpLeft, 'a'/'b' HalfSlopeUpRight Low/High,
//   'c'/'d' HalfSlopeUpLeft Low/High, 'H' ladder (flag), '^' Solid + hazard, '~' hazard only,
//   'o' opaque only (walkable), 'W' Solid + opaque; anything else is Empty. Tile size 16, origin 0.
inline TileWorld grid_from(std::initializer_list<std::string_view> rows, int tile = 16, Vec2 origin = {}) {
    TileWorld w;
    const int height = static_cast<int>(rows.size());
    const int width = height > 0 ? static_cast<int>(rows.begin()->size()) : 0;
    w.resize(width, height, tile, origin);
    int y = 0;
    for (const std::string_view row : rows) {
        REQUIRE(static_cast<int>(row.size()) == width);
        for (int x = 0; x < width; ++x) {
            TileShape s = TileShape::Empty;
            std::uint8_t f = 0;
            switch (row[static_cast<std::size_t>(x)]) {
            case '#': s = TileShape::Solid; break;
            case '=': s = TileShape::OneWay; break;
            case '/': s = TileShape::SlopeUpRight; break;
            case '\\': s = TileShape::SlopeUpLeft; break;
            case 'a': s = TileShape::HalfSlopeUpRightLow; break;
            case 'b': s = TileShape::HalfSlopeUpRightHigh; break;
            case 'c': s = TileShape::HalfSlopeUpLeftLow; break;
            case 'd': s = TileShape::HalfSlopeUpLeftHigh; break;
            case 'H': f = kTileLadder; break;
            case '^': s = TileShape::Solid; f = kTileHazard; break;
            case '~': f = kTileHazard; break;
            case 'o': f = kTileOpaque; break;
            case 'W': s = TileShape::Solid; f = kTileOpaque; break;
            default: break;
            }
            w.set({x, y}, s, f);
        }
        ++y;
    }
    return w;
}

inline TileWorld open_grid(int width, int height, int tile = 16) {
    TileWorld w;
    w.resize(width, height, tile, {});
    return w;
}

// A box standing on feet (x, y): [x - w/2, x + w/2) x [y - h, y).
inline Rect feet_box(float x, float y, float w, float h) { return Rect{x - w * 0.5f, y - h, w, h}; }

}
