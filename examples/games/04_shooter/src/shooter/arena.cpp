// Arena setup: TileWorld from the ASCII layout, the Arena/Crowd resources and the player.
#include <shooter/arena.h>

#include <lam/core/assert.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/tile_world.h>
#include <shooter/components.h>
#include <shooter/crowd.h>
#include <shooter/ids.h>

namespace shooter {
using namespace lam;

std::uint32_t wave_size(std::uint32_t w, std::uint32_t stress) {
    if (stress > 0) return stress;
    const std::uint32_t k = w > 0 ? w - 1 : 0;
    return 16 + 14 * k + 2 * k * k;                             // 16, 32, 52, 76, 104 ... 304 (w10)
}

Vec2 tile_center(int x, int y) {
    return {(static_cast<float>(x) + 0.5f) * kTileSize, (static_cast<float>(y) + 0.5f) * kTileSize};
}

Vec2 player_start() {
    for (int y = 0; y < kArenaH; ++y)
        for (int x = 0; x < kArenaW; ++x)
            if (kArenaRows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] == 'P')
                return tile_center(x, y);
    return tile_center(kArenaW / 2, kArenaH / 2);
}

void build_arena_tiles(World &w) {
    // Filled by hand instead of from a Tilemap.
    TileWorld &tiles = w.resources().get<TileWorld>();
    tiles.resize(kArenaW, kArenaH, kTileSize, Vec2{0, 0});
    tiles.outside_solid = true;
    for (int y = 0; y < kArenaH; ++y) {
        const std::string_view row = kArenaRows[static_cast<std::size_t>(y)];
        LAM_CHECK(row.size() == static_cast<std::size_t>(kArenaW), "arena row has the wrong width");
        for (int x = 0; x < kArenaW; ++x) {
            if (row[static_cast<std::size_t>(x)] != '#') continue;
            tiles.set({x, y}, TileShape::Solid, kTileOpaque);
            tiles.cost(x, y) = 0;                               // impassable for the flow field
        }
    }
}

Entity find_player(World &w) {
    for (auto [e, t] : w.view<Transform>(With<Player>{})) return e;
    return {};
}

Entity setup_arena(App &app, World &w, const ArenaConfig &config) {
    build_arena_tiles(w);
    Arena &arena = w.resources().insert<Arena>();
    arena.stress_enemies = config.stress_enemies;
    arena.god = config.god;
    w.snapshot_resource<Arena>();
    Crowd &crowd = w.resources().insert<Crowd>();
    crowd.distance.resize({kArenaW, kArenaH});                  // allocate once, at load time
    // Immediate spawn (not while iterating): the player exists before the first tick.
    return app.res<PrefabLibrary>().spawn(w, ids::kPrefabPlayer, player_start());
}
}
