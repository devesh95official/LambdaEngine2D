#pragma once
// Private per-world state of TilemapPlugin (a World resource inserted by init_world) and the
// TileWorld cell rule shared by fill_tile_world, set_tile and the restore hook.
#include <cstdint>
#include <span>

#include <lam/ecs/entity.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/tilemap.h>

namespace lam {
class App;
namespace detail {

struct TilemapWorldState {
    App *app = nullptr;               // owner of the world (AssetServer, config)
    Entity collision_map;             // the Tilemap entity whose instance filled TileWorld
    std::uint32_t tilesets_stamp = 0; // tileset versions when TileWorld was filled
    std::uint64_t anim_ticks = 0;     // animated_tiles clock (presentation; never hashed)
    int tick_rate = 60;
    bool movers_set_busy = false; // grid_movers set TurnScheduler::busy
    bool warned_many_maps = false;
    bool warned_path = false; // a failed path request was logged
};

struct CellProps {
    TileShape shape = TileShape::Empty;
    std::uint8_t flags = 0;
    std::uint16_t cost = 10;
};

// fill_tile_world's rule for one cell over the collision layers (`only` non-null: that layer alone,
// collision flag ignored).
CellProps tile_world_cell(const TilemapAsset &m, std::span<const TileLayer> layers,
                          std::span<const Tileset *const> sets, int x, int y,
                          const TileLayer *only = nullptr);

} // namespace detail
} // namespace lam
