#pragma once
// The orchard board: walls and the traveller's start cell, from a native map (the engine's
// TilemapAsset). Both board files go through the engine's parser (lam::parse_native_map):
//   levels/orchard.map.json  native JSON: terrains "wall" (solid, opaque, cost 0) and "grass",
//                            ASCII rows, and a legend entry 'P' = grass + a "start" object;
//   levels/orchard.txt       the same board as raw ASCII rows ('#' is a solid wall,
//                            every other character an Empty terrain named by itself, 'P' the start).
// The walls go once into a TileWorld prototype (lam::fill_tile_world) that each run copies into its
// world. There is no Tilemap component, so the tilemap module never touches it.
#include <string_view>
#include <lam/core/result.h>
#include <lam/math/vec2.h>
#include <lam/physics/tile_world.h>

namespace lam {
struct TilemapAsset;
}

namespace trail {

struct Orchard {
    lam::TileWorld tiles;                                       // walls Solid + opaque, cost 0; origin (0, 0)
    lam::IVec2 start{-1, -1};                                   // traveller start (faces right)
    int width() const { return tiles.shapes.width(); }
    int height() const { return tiles.shapes.height(); }
    bool wall(lam::IVec2 c) const { return tiles.at(c) != lam::TileShape::Empty; }   // outside = wall
};

// From a parsed or loaded map. The start is the first "start" object, else the first cell of
// terrain "P" (raw ASCII boards), else the middle of the board. The orchard only uses its
// embedded terrain set, so no AssetServer is needed.
lam::Result<Orchard> orchard_from_map(const lam::TilemapAsset &map, std::string_view file_name);
// The same from file text (JSON or raw ASCII rows), through lam::parse_native_map (tests, tools).
lam::Result<Orchard> parse_orchard(std::string_view text, std::string_view file_name);

} // namespace trail
