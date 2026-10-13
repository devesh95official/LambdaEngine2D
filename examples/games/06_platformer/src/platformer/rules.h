#pragma once
// The rules plugin and the level spawner shared by the game and its tests. Rules only run on
// worlds holding a Level, so menu and overlay worlds are left alone.
#include <cstdint>
#include <string>
#include <string_view>
#include <lam/core/app.h>
#include <lam/ecs/world.h>
#include "platformer/components.h"

namespace plat {
// Map loaded by spawn_level (relative to the asset root).
inline constexpr std::string_view kLevelMap = "maps/level1.tmj";
// Tick counts used by the rules (60 Hz).
inline constexpr std::uint16_t kInvulnTicks = 90;
inline constexpr float kStompBounce = 230;                      // px/s upwards after a stomp
inline constexpr int kStartLives = 3;

// Components, events, Controls and the rule systems. Also registers BurstAt/PlaySound so the
// rules can emit them in tests.
class PlatformerPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "game.platformer"; }
    void build(lam::App &app) override;
};

// Inserts the Level resource and a Tilemap entity; tilemap_sync then fills TileWorld and spawns
// the map objects as prefabs. Call from Scene::enter, or on the main world in tests.
void spawn_level(lam::App &app, lam::World &world, std::string_view map_path = kLevelMap);

// coins x 100 + stomps x 250 + 1000 for the goal + 200 per life left at the goal.
std::int32_t score_of(const Level &level);

// "platformer summary: ticks=<n> score=<s> coins=<c>/<t> stomps=<k> deaths=<d> lives=<l> goal=<0|1>
// hash=<16 hex>" for a level world (zeros when `world` has no Level). Allocates (exit path only).
std::string summary_line(const lam::World &world);
}
