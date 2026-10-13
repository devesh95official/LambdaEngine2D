#pragma once
// Simulation types, the rules plugin and the bot (top_down_shooter_sim, no raylib).
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/math/vec2.h>
#include <lam/particles/components.h>
namespace game {

// TODO(level): draw your arena. '#' wall, '.' floor, 'S' enemy spawn pad, 'P' player start.
// Collision comes from these rows. tools/gen_assets.py reads them (between the markers) to write
// the drawn Tiled map, so rerun it after editing.
inline constexpr int kTile = 16;
inline constexpr int kArenaW = 36;
inline constexpr int kArenaH = 22;
// ARENA-BEGIN
inline constexpr std::array<std::string_view, kArenaH> kArena = {
    "####################################",
    "#S................................S#",
    "#..................................#",
    "#..................................#",
    "#.....####................####.....#",
    "#..................................#",
    "#..................................#",
    "#..................................#",
    "#...........##........##...........#",
    "#...........##........##...........#",
    "#S.................................#",
    "#................P................S#",
    "#...........##........##...........#",
    "#...........##........##...........#",
    "#..................................#",
    "#..................................#",
    "#..................................#",
    "#.....####................####.....#",
    "#..................................#",
    "#..................................#",
    "#S................................S#",
    "####################################",
};
// ARENA-END
constexpr char arena_at(int x, int y) {
    return kArena[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
}

// Collision layers (Collider::layer, Projectile::mask). Must match assets/prefabs.json.
inline constexpr std::uint32_t kLayerPlayer = 2;
inline constexpr std::uint32_t kLayerEnemy = 4;

// ---- Components (named in prefabs.json) ----------------------------------------------------------
struct Player {};                                               // tag
struct Health {
    std::int32_t hp = 3;
    std::int32_t max_hp = 3;
    std::uint16_t invuln = 0;                                   // ticks of immunity after a hit
};
struct Enemy { std::int32_t points = 10; };                     // TODO(enemies): kinds, damage...
struct Gun {
    lam::Vec2 aim{1, 0};                                        // unit; set from input each tick
    bool trigger = false;                                       // intent: wants to shoot this tick
    std::uint16_t cooldown = 0;                                 // ticks until the next shot
    std::uint16_t fire_ticks = 10;                              // ticks between shots (prefab data)
    float bullet_speed = 300;                                   // px/s
};

// World resource, snapshotted in start_run. Timers are in ticks.
struct Run {
    std::int32_t score = 0;
    std::int32_t best = 0;
    std::uint32_t kills = 0;
    std::uint32_t deaths = 0;
    std::uint32_t spawn_timer = 60;                             // ticks until the next enemy
    bool over = false;                                          // the player died; fire restarts
};

// App resources. Fx is filled by the executable; tests leave it out, so rules use try_res.
struct Controls { lam::AxisAction move; lam::ButtonAction fire; };
struct Fx { lam::AssetId<lam::EmitterConfig> pop; };
inline constexpr lam::StrId kSndPop = lam::str_id("pop");      // sound bank group (main.cpp)

// Every rule system is gated on Run, so menu worlds added later skip them. Add after DefaultPlugins.
class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "game.rules"; }
    void build(lam::App &app) override;
};

// Builds the walls from kArena, inserts Run and spawns the "player" prefab. Needs prefabs loaded.
lam::Entity start_run(lam::App &app, lam::World &w);
lam::Entity find_player(lam::World &w);
lam::Vec2 cell_center(lam::IVec2 cell);

// --bot: sets the same InputFrame bits a player would.
class Bot final : public lam::InputDriver {
public:
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
};
}
// Reflection: global scope, qualified names.
LAM_REFLECT_TAG(game::Player)
LAM_REFLECT(game::Health, hp, max_hp, invuln)
LAM_REFLECT(game::Enemy, points)
LAM_REFLECT(game::Gun, aim, trigger, cooldown, fire_ticks, bullet_speed)
LAM_REFLECT(game::Run, score, best, kills, deaths, spawn_timer, over)
