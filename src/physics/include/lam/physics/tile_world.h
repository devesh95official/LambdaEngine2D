#pragma once
// Tile collision grid, filled by the tilemap or by hand. Shape and flags are separate so a tile
// can be solid and a hazard. Everything but Empty and OneWay blocks top-down movement.
// An empty (0 x 0) world has no tiles at all and ignores outside_solid.
#include <cstdint>
#include <optional>
#include <span>
#include <lam/math/grid.h>
#include <lam/math/rect.h>
#include <lam/math/vec2.h>
namespace lam {
enum class TileShape : std::uint8_t { Empty, Solid, OneWay, SlopeUpRight, SlopeUpLeft, HalfSlopeUpRightLow,
    HalfSlopeUpRightHigh, HalfSlopeUpLeftLow, HalfSlopeUpLeftHigh };
enum TileFlags : std::uint8_t { kTileHazard = 1, kTileLadder = 2, kTileWater = 4, kTileOpaque = 8 };
enum TileSides : std::uint8_t { kSideLeft = 1, kSideRight = 2, kSideTop = 4, kSideBottom = 8 };
// Path cost of a cell when nothing else sets one.
inline constexpr std::uint16_t kDefaultTileCost = 10;
struct TileWorld {
    Grid<TileShape> shapes;
    Grid<std::uint8_t> flags;                                   // TileFlags
    Grid<std::uint16_t> cost;                                   // pathfinding; 0 = impassable; default 10
    int tile_size = 16;
    Vec2 origin;                                                // top-left of cell (0, 0)
    bool outside_solid = true;                                  // outside the grid is Solid and opaque
    // TileSides that stay open past the edge. Platformers want kSideBottom so pits aren't floored.
    std::uint8_t open_sides = 0;
    std::uint32_t version = 0;                                  // bumped on every change
    void resize(int width, int height, int tile, Vec2 world_origin) {
        shapes.resize(width, height, TileShape::Empty);
        flags.resize(width, height, 0);
        cost.resize(width, height, kDefaultTileCost);
        tile_size = tile > 0 ? tile : 1;
        origin = world_origin;
        ++version;
    }
    IVec2 tile_at(Vec2 p) const { return floor_to_ivec((p - origin) / static_cast<float>(tile_size)); }
    Rect tile_rect(IVec2 t) const {
        const float s = static_cast<float>(tile_size);
        return Rect{origin.x + static_cast<float>(t.x) * s, origin.y + static_cast<float>(t.y) * s, s, s};
    }
    Vec2 tile_center(IVec2 t) const { return tile_rect(t).center(); }
    bool has_cells() const { return shapes.width() > 0 && shapes.height() > 0; }
    // Outside the grid and not past an open side.
    bool outside_closed(IVec2 t) const {
        if (!outside_solid || !has_cells()) return false;
        const std::uint8_t beyond =
            static_cast<std::uint8_t>((t.x < 0 ? kSideLeft : 0) | (t.x >= shapes.width() ? kSideRight : 0) |
                                      (t.y < 0 ? kSideTop : 0) | (t.y >= shapes.height() ? kSideBottom : 0));
        return (beyond & open_sides) == 0;
    }
    TileShape at(IVec2 t) const {
        if (shapes.inside(t)) return shapes(t.x, t.y);
        return outside_closed(t) ? TileShape::Solid : TileShape::Empty;
    }
    std::uint8_t flags_at(IVec2 t) const {
        if (flags.inside(t)) return flags(t.x, t.y);
        return static_cast<std::uint8_t>(outside_closed(t) ? kTileOpaque : 0);
    }
    bool opaque(IVec2 t) const { return (flags_at(t) & kTileOpaque) != 0; }
    // Leaves `cost` alone, so a hand-built wall also needs cost 0 for pathfinding.
    void set(IVec2 t, TileShape s, std::uint8_t f) {
        if (!shapes.inside(t)) return;
        shapes(t.x, t.y) = s;
        flags(t.x, t.y) = f;
        ++version;
    }
    // Also sets the path cost (0 = impassable).
    void set(IVec2 t, TileShape s, std::uint8_t f, std::uint16_t path_cost) {
        if (!shapes.inside(t)) return;
        shapes(t.x, t.y) = s;
        flags(t.x, t.y) = f;
        if (cost.inside(t)) cost(t.x, t.y) = path_cost;
        ++version;
    }
};
struct MoveBoxOptions {
    float corner_assist = 0;                                    // top-down, px; single-axis moves only
    bool platformer = false;                                    // slopes and one-way platforms
    bool drop_through = false;
    float snap_down = 0;                                        // platformer: keep ground contact down slopes
    std::span<const Rect> solids = {};                          // entity boxes that block
    std::span<const Rect> one_way = {};                         // entity platform tops
};
struct BoxMove {
    Rect box;
    bool hit_x = false, hit_y = false, grounded = false, on_slope = false;
    Vec2 ground_normal{0, -1};
    std::int32_t ground_solid = -1;                             // index into solids/one_way (solids first)
    std::int32_t blocking_solid = -1;
};
// X then Y sweep against exact tile edges; boxes here are absolute world rects. A box that starts
// stuck can walk out in top-down mode but doesn't move at all in platformer mode.
BoxMove move_box(const TileWorld &w, Rect box, Vec2 delta, const MoveBoxOptions &o = {});
bool box_blocked(const TileWorld &w, Rect box, std::span<const Rect> solids = {});
// Top-left of the nearest clear box on a half-tile raster, searching up to max_tiles out.
// Returns the box corner, not the anchor; add (position - box.min()) to place a body.
std::optional<Vec2> nearest_free_spot(const TileWorld &w, Rect box, int max_tiles);
bool tile_ray_clear(const TileWorld &w, Vec2 a, Vec2 b);        // Amanatides-Woo
// First blocking cell from a to b (or first opaque one with opaque_only).
std::optional<IVec2> tile_raycast(const TileWorld &w, Vec2 a, Vec2 b, bool opaque_only = false);
}
