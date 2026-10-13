#pragma once
// Breakout rules: components, the run resource, the rules plugin and the bot (breakout_sim).
// Everything here is pure (no raylib), so it runs in tests and under --headless.
#include <cstdint>
#include <span>
#include <string_view>
#include <lam/core/app.h>
#include <lam/ecs/component.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/math/vec2.h>

namespace breakout {
// Playfield in world pixels (= virtual pixels: there is no camera).
inline constexpr float kFieldW = 320;
inline constexpr float kFieldH = 240;
inline constexpr float kWall = 8;                               // wall thickness (left, right, top)
inline constexpr float kPaddleY = 222;                          // paddle centre
inline constexpr lam::Vec2 kPaddleHalf{20, 4};
inline constexpr float kBallRadius = 3;
inline constexpr int kCols = 10;
inline constexpr int kRows = 5;
inline constexpr lam::Vec2 kBrickHalf{14, 5};
inline constexpr float kBrickTop = 36;                          // centre y of row 0
inline constexpr float kBrickPitchY = 12;
inline constexpr float kBrickPitchX = 30;
inline constexpr std::int32_t kStartLives = 3;

// Collision layers.
inline constexpr std::uint32_t kLayerWall = 1;                  // TopDownMotor::solid_mask default
inline constexpr std::uint32_t kLayerBall = 2;
inline constexpr std::uint32_t kLayerPaddle = 4;
inline constexpr std::uint32_t kLayerBrick = 8;

struct Paddle {};
struct Wall {};
struct Ball {};
struct Brick { std::int32_t row = 0; std::int32_t points = 10; };

// World resource of the play world (snapshotted: undo, saves, state_hash).
struct Run {
    std::int32_t score = 0;
    std::int32_t lives = kStartLives;
    std::int32_t level = 1;
    std::int32_t bricks_left = 0;
    std::int32_t paddle_hits = 0;
    std::int32_t bricks_broken = 0;
    std::int32_t balls_lost = 0;
    bool serving = true;                                        // ball rides on the paddle
    bool over = false;                                          // no lives left
};
// Across restarts (also snapshotted).
struct Stats {
    std::int32_t games = 1;
    std::int32_t best_score = 0;
    std::int32_t total_bricks = 0;
};
// App resource: action ids resolved once.
struct Controls { lam::AxisAction move; lam::ButtonAction launch; };
}
LAM_REFLECT_TAG(breakout::Paddle)
LAM_REFLECT_TAG(breakout::Wall)
LAM_REFLECT_TAG(breakout::Ball)
LAM_REFLECT(breakout::Brick, row, points)
LAM_REFLECT(breakout::Run, score, lives, level, bricks_left, paddle_hits, bricks_broken, balls_lost, serving, over)
LAM_REFLECT(breakout::Stats, games, best_score, total_bricks)

namespace breakout {
// Registers components, events, controls (needs InputPlugin) and the rules (needs PhysicsPlugin).
class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "breakout.rules"; }
    void build(lam::App &app) override;
};

// Fills a world with walls, paddle, ball and a full wall of bricks; inserts Run and Stats.
void start_game(lam::World &w);
float ball_speed(std::int32_t level);                           // px/s

// The bot's decision: a pure function of world state.
struct BotOut { float move_x = 0; bool launch = false; };
BotOut bot_decide(lam::World &w);

class Bot final : public lam::InputDriver {
public:
    Controls controls;                                          // copied from the App resource
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
};
}
