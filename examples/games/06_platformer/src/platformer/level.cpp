// Level spawning, scoring and the headless summary line.
#include <cinttypes>
#include <cstdio>
#include <lam/assets/asset_server.h>
#include <lam/ecs/transform.h>
#include <lam/tilemap/tilemap.h>
#include "platformer/rules.h"

namespace plat {
using namespace lam;

void spawn_level(App &app, World &world, std::string_view map_path) {
    // Snapshotted, so undo, saves and the state hash cover it.
    world.resources().insert<Level>(Level{.lives = kStartLives});
    world.snapshot_resource<Level>();
    // The map entity; its Transform is the map origin. Everything else in the level is a Tiled
    // object, spawned as a prefab once the map is Ready.
    Entity map = world.create();
    world.add<Transform>(map);
    world.add<Tilemap>(map, Tilemap{.map = app.res<AssetServer>().load<TilemapAsset>(map_path)});
}

std::int32_t score_of(const Level &level) {
    std::int32_t s = level.coins * 100 + level.stomps * 250;
    if (level.complete) s += 1000 + level.lives * 200;
    return s;
}

std::string summary_line(const World &world) {
    const Level *lv = world.resources().try_get<Level>();
    const Level empty{.lives = 0};
    const Level &l = lv ? *lv : empty;
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "platformer summary: ticks=%" PRIu64 " score=%d coins=%d/%d stomps=%d deaths=%d lives=%d "
                  "goal=%d hash=%016" PRIx64,
                  world.tick(), static_cast<int>(score_of(l)), static_cast<int>(l.coins),
                  static_cast<int>(l.coins_total), static_cast<int>(l.stomps), static_cast<int>(l.deaths),
                  static_cast<int>(l.lives), l.complete ? 1 : 0, world.state_hash());
    return buf;
}
}
