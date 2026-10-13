#pragma once
// Simulation types, the rules plugin and the helpers main.cpp and the tests use (platformer_sim).
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/math/vec2.h>
#include <lam/particles/components.h>
#include <lam/physics/tile_world.h>

// Global scope: LAM_REFLECT needs it, and prefabs refer to components by name.
struct Player {};                                               // tag: the hero ("tags": ["Player"])
struct Coin {};                                                 // tag: a trigger the hero collects
struct Goal {};                                                 // tag: a trigger that ends the level
// TODO: your components here (enemies, keys, checkpoints...). Reflect and register each one.

// World resource of the level world, snapshotted in start_level so undo, saves and the hash
// cover it.
struct Level {
    std::int32_t coins = 0, coins_total = 0, falls = 0;
    lam::Vec2 respawn;                                          // hero feet position at level start
    float kill_y = 1e9f;                                        // feet below this line = fell in a pit
    bool started = false;                                       // set on the first tick with a hero
    bool won = false;
};

// App resources. Fx is filled by presentation; tests leave it out and get no BurstAt.
struct Controls { lam::AxisAction move; lam::ButtonAction jump; };
struct Fx { lam::AssetId<lam::EmitterConfig> sparkle; };

// Emitted once when the level ends; main.cpp shows the result screen.
struct LevelFinished { bool won = true; };                      // TODO: won = false for a game over

LAM_REFLECT_TAG(Player)
LAM_REFLECT_TAG(Coin)
LAM_REFLECT_TAG(Goal)
LAM_REFLECT(Level, coins, coins_total, falls, respawn, kill_y, started, won)

namespace platformer {
inline constexpr std::string_view kLevelMap = "maps/level1.tmj";   // relative to assets/

// Registers the game's components, events, controls and rule systems (see rules.cpp).
class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "platformer.rules"; }
    void build(lam::App &app) override;
};

// Adds the Level resource and a Tilemap entity. The tilemap module fills TileWorld and spawns the
// map's objects as prefabs.
void start_level(lam::App &app, lam::World &world, std::string_view map = kLevelMap);

// "platformer summary: ticks=<n> coins=<c>/<t> falls=<f> won=<0|1> hash=<16 hex>".
std::string summary_line(const lam::World &world);

// Autoplay: runs right and jumps at walls, spikes and pits. bot_should_jump only reads the
// TileWorld, so it can be tested without physics.
bool bot_should_jump(const lam::TileWorld &tiles, lam::Vec2 feet, float half_width);
class Bot final : public lam::InputDriver {
public:
    Controls controls;                                          // copy of the App resource
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
private:
    int hold_ticks_ = 0, stuck_ticks_ = 0;
};
}
