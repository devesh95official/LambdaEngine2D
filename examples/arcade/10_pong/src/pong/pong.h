#pragma once
// Pong rules. The whole match is one snapshotted resource, Match.
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
namespace pong {

// Field geometry in pixels; world and screen are both 480x270.
inline constexpr float kW = 480, kH = 270;
inline constexpr float kTop = 22, kBottom = 262;                // play area between the walls
inline constexpr float kPadW = 6, kPadH = 36, kPadInset = 16;
inline constexpr float kBall = 6;
inline constexpr float kLeftFront = kPadInset + kPadW;          // left paddle's hitting face
inline constexpr float kRightFront = kW - kPadInset - kPadW;    // right paddle's hitting face
inline constexpr int kWinScore = 7;

enum Phase : std::int32_t { kTitle = 0, kServe = 1, kPlay = 2, kOver = 3 };

struct Match {
    std::int32_t phase = kTitle;
    bool two_player = false;
    float p1 = (kTop + kBottom - kPadH) / 2, p2 = (kTop + kBottom - kPadH) / 2;   // paddle tops
    float prev_p1 = p1, prev_p2 = p2;                           // for interpolated drawing
    lam::Vec2 ball{(kW - kBall) / 2, (kTop + kBottom - kBall) / 2};   // top-left
    lam::Vec2 prev_ball = ball;
    lam::Vec2 vel{};                                            // px per tick
    float speed = 0;
    float ai_error = 0;                                         // CPU aim offset, re-rolled each hit
    float mouse_y = -1;                                         // last pointer y
    bool mouse_mode = false;
    std::int32_t score1 = 0, score2 = 0;
    std::int32_t serve_dir = 1;                                 // +1 toward the right player
    std::int32_t winner = 0;                                    // 1 or 2 when over
    std::uint32_t phase_ticks = 0;
    std::uint32_t rally = 0, best_rally = 0;                    // shown as the high score
    std::uint32_t hits = 0, points1 = 0, points2 = 0, matches = 0, wins = 0;
};

// App resources. Fx is filled by the executable and may be missing.
struct Controls {
    lam::ButtonAction p1_up, p1_down, p2_up, p2_down, start, mode, title;
};
struct Fx { lam::AssetId<lam::EmitterConfig> hit1, hit2, goal; };

class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "pong.rules"; }
    void build(lam::App &app) override;
};

// Adds Match to the world and snapshots it.
void start(lam::World &w);

// --bot: plays the left paddle and starts new matches.
class Bot final : public lam::InputDriver {
public:
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
};
}
LAM_REFLECT(pong::Match, phase, two_player, p1, p2, prev_p1, prev_p2, ball, prev_ball, vel, speed,
            ai_error, mouse_y, mouse_mode, score1, score2, serve_dir, winner, phase_ticks, rally,
            best_rally, hits, points1, points2, matches, wins)
