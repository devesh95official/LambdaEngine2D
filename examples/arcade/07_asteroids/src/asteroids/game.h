#pragma once
// Asteroids rules: components, run state, the rules plugin and the bot. No raylib here.
#include <cstdint>
#include <span>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/math/vec2.h>
#include <lam/particles/components.h>

namespace ast {
inline constexpr float kW = 640, kH = 360;                     // playfield is the whole screen

enum Phase : std::int32_t { kTitle = 0, kPlay = 1, kOver = 2 };

struct Body { lam::Vec2 vel; float radius = 8; };              // wraps at the screen edges
struct Ship {
    std::uint16_t invuln = 0;                                   // ticks of immunity after respawn
    std::uint16_t cooldown = 0;                                 // ticks until the next shot
    bool thrusting = false;
};
struct Rock { std::int32_t size = 3; std::uint32_t shape = 0; };   // 3 large, 2 medium, 1 small; 0 = dead
struct Bullet {};

struct Run {                                                    // snapshotted World resource
    std::int32_t phase = kTitle;
    std::int32_t score = 0, best = 0, lives = 3, wave = 0;
    std::int32_t next_life = 10000;                             // score that gives an extra ship
    std::uint32_t respawn = 0;                                  // ticks until the ship returns
    std::uint32_t wave_delay = 0;                               // ticks until the next wave
    std::uint32_t over_ticks = 0;                               // ticks spent on the game-over screen
    std::uint32_t kills = 0, deaths = 0, games = 0;
};

struct Controls {                                               // App resource
    lam::ButtonAction left, right, thrust, fire, start;
};
struct Fx { lam::AssetId<lam::EmitterConfig> boom, spark, exhaust; };   // App resource, filled by main.cpp

inline float rock_radius(std::int32_t size) { return size >= 3 ? 26.0f : size == 2 ? 15.0f : 8.0f; }
// Shortest offset from a to b on the wrapping playfield.
lam::Vec2 wrap_delta(lam::Vec2 a, lam::Vec2 b);
lam::Entity find_ship(lam::World &w);
void start_world(lam::World &w);                                // adds Run to the world

class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "asteroids.rules"; }
    void build(lam::App &app) override;
};

class Bot final : public lam::InputDriver {
public:
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
};
}
LAM_REFLECT(ast::Body, vel, radius)
LAM_REFLECT(ast::Ship, invuln, cooldown, thrusting)
LAM_REFLECT(ast::Rock, size, shape)
LAM_REFLECT_TAG(ast::Bullet)
LAM_REFLECT(ast::Run, phase, score, best, lives, wave, next_life, respawn, wave_delay, over_ticks, kills,
            deaths, games)
