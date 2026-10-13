#pragma once
// The forest level: the ASCII file (assets/levels/forest.txt) parsed into a Level and
// copied into the world's TileWorld, which every physics and steering helper reads.
//   '#' forest edge, 'T' tree, 'R' rock (all solid and opaque), 'P' start, 'W' wolf den.
// TileWorld is filled by hand so the rules don't depend on a tilemap asset; the executable still
// draws the ground from a Tiled map.
#include <cstdint>
#include <string_view>
#include <vector>
#include <lam/core/result.h>
#include <lam/math/vec2.h>

namespace lam {
class World;
struct TileWorld;
}

namespace lantern {

inline constexpr int kTile = 16;                                // pixels per tile

struct Level {                                                  // World resource; constant after load
    int width = 0, height = 0;                                  // tiles
    lam::IVec2 start{2, 2};
    std::vector<lam::IVec2> dens;
    std::vector<lam::IVec2> trees;
    std::vector<lam::IVec2> rocks;
    std::vector<std::uint8_t> solid;                            // width * height, row-major
    bool solid_at(lam::IVec2 t) const;                          // outside = solid
};

lam::Result<Level> parse_level(std::string_view text);

// Feet stand at the bottom-centre of their tile.
lam::Vec2 tile_feet(lam::IVec2 tile);

// Shapes, opacity and path costs (0 = blocked) for every cell.
void fill_tile_world(const Level &level, lam::TileWorld &out);

// Parses `text`, inserts the Level resource, fills the world's TileWorld (inserting one when no
// physics plugin did) and spawns the trees and rocks (prefabs "tree"/"rock" when the world's
// PrefabLibrary has them, else bare entities). Load time; allocates.
lam::Status load_level(lam::World &w, std::string_view text);

} // namespace lantern
