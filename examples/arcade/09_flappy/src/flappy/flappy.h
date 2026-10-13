#pragma once
// Flappy rules. Run holds the phase and score; the bird and each pipe pair are entities.
// Motion uses fixed per-tick constants so replays match.
#include <cstdint>
#include <span>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/math/vec2.h>
#include <lam/particles/components.h>

namespace flappy {
// Playfield in pixels. The camera is centred at zoom 1, so world and screen match.
inline constexpr float kW = 240;
inline constexpr float kH = 360;
inline constexpr float kGroundY = 318;                          // top of the ground strip
inline constexpr float kBirdX = 70;
inline constexpr float kBirdR = 6;                              // collision radius
inline constexpr float kStartY = 160;
inline constexpr float kPipeW = 36;
inline constexpr float kGap = 86;                               // vertical opening of a pair
inline constexpr float kScroll = 1.5f;                          // px per tick (90 px/s)
inline constexpr std::uint32_t kPipeEvery = 92;                 // ticks between pairs (138 px)
inline constexpr float kGravity = 0.25f;                        // px per tick^2
inline constexpr float kFlapVy = -4.6f;                         // px per tick
inline constexpr float kMaxFall = 7.0f;
inline constexpr std::uint32_t kRestartDelay = 40;              // ticks before a flap restarts

enum Phase : std::int32_t { kTitle = 0, kPlay = 1, kOver = 2 };

struct Bird { float vy = 0; };
struct Pipe { float gap_y = 180; bool passed = false; };

// Snapshotted World resource.
struct Run {
    std::int32_t phase = kTitle;
    std::int32_t score = 0;
    std::int32_t best = 0;                                      // kept across restarts
    std::int32_t games = 0;
    std::int32_t deaths = 0;
    std::int32_t flaps = 0;
    std::uint32_t phase_ticks = 0;
    std::uint32_t scroll = 0;                                   // ticks the world has scrolled
    std::uint32_t next_pipe = 0;
};

struct Controls { lam::ButtonAction flap; };                    // App resource
struct Fx { lam::AssetId<lam::EmitterConfig> feathers, sparkle, puff; };   // App resource, filled by main.cpp
}
LAM_REFLECT(flappy::Bird, vy)
LAM_REFLECT(flappy::Pipe, gap_y, passed)
LAM_REFLECT(flappy::Run, phase, score, best, games, deaths, flaps, phase_ticks, scroll, next_pipe)

namespace flappy {
class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "flappy.rules"; }
    void build(lam::App &app) override;
};

void start_game(lam::World &w);                                 // Run + the bird
lam::Entity find_bird(lam::World &w);

// --bot: flaps when the bird sinks below the next gap.
class Bot final : public lam::InputDriver {
public:
    Controls controls;
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
};
}
