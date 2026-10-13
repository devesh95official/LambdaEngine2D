#pragma once
// 2048 rules: slide/merge on a plain 16-cell array, plus the Game resource, tick system and bot.
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/math/rect.h>
#include <lam/math/vec2.h>

namespace lam {
struct EmitterConfig;
class World;
}

namespace g2048 {

inline constexpr int kN = 4;
inline constexpr int kCells = kN * kN;
inline constexpr int kSlideTicks = 7;                           // slide tween length
inline constexpr int kPopTicks = 8;                             // merge/spawn pop after the slide
inline constexpr int kWinValue = 2048;

// Layout in screen pixels (480x270). The rules use it for bursts and button hit tests.
inline constexpr lam::Vec2 kVirtual{480, 270};
inline constexpr float kTile = 52, kGap = 6;
inline constexpr float kBoardSize = kN * kTile + (kN + 1) * kGap;   // 238
inline constexpr lam::Vec2 kBoardPos{26, 16};
inline constexpr lam::Rect kNewButton{300, 186, 76, 22};
inline constexpr lam::Rect kUndoButton{384, 186, 76, 22};
inline lam::Vec2 cell_pos(int idx) {                            // top-left of a cell
    return {kBoardPos.x + kGap + static_cast<float>(idx % kN) * (kTile + kGap),
            kBoardPos.y + kGap + static_cast<float>(idx / kN) * (kTile + kGap)};
}
inline lam::Vec2 cell_center(int idx) { return cell_pos(idx) + lam::Vec2{kTile / 2, kTile / 2}; }

using Cells = std::array<std::int32_t, kCells>;                 // 0 = empty, else the tile value

// ---- Board logic --------------------------------------------------------------------------------
enum Dir : int { kUp = 0, kRight = 1, kDown = 2, kLeft = 3 };
struct Slide {
    Cells cells{};
    Cells src_a{}, src_b{};                                     // where each cell came from, -1 if nowhere
    std::int32_t gained = 0;                                    // points from merges
    std::int32_t merges = 0;
    bool moved = false;
};
Slide slide(const Cells &cells, int dir);
bool can_move(const Cells &cells);
int max_tile(const Cells &cells);
int best_dir(const Cells &cells);                               // greedy pick for the bot, -1 if stuck

// ---- Game ---------------------------------------------------------------------------------------
enum class Phase : std::uint8_t { Title, Play, Won, Over };
struct Game {                                                   // snapshotted World resource
    Cells cells{}, prev{};
    Cells src_a{}, src_b{};
    std::int32_t score = 0, prev_score = 0, best = 0;
    std::int32_t moves = 0, games = 0, spawned = -1;
    std::int32_t since_move = 1000;                             // ticks since the last slide
    std::int32_t phase_ticks = 0;
    std::int32_t drag_x = 0, drag_y = 0;                        // where the mouse went down
    bool can_undo = false, keep_going = false, dragging = false;
    Phase phase = Phase::Title;
};
struct Fx { lam::AssetId<lam::EmitterConfig> pop, confetti; };  // App resource, may be missing
struct Controls {                                               // App resource
    lam::ButtonAction up, right, down, left, undo, restart, confirm, click;
};

void init_game(lam::World &w);                                  // adds and snapshots Game

class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "g2048.rules"; }
    void build(lam::App &app) override;
};

// --bot: greedy one-move lookahead.
class Bot final : public lam::InputDriver {
public:
    explicit Bot(Controls c) : c_(c) {}
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
private:
    Controls c_;
};

}  // namespace g2048

LAM_REFLECT_ENUM(g2048::Phase, "title", "play", "won", "over")
LAM_REFLECT(g2048::Game, cells, prev, src_a, src_b, score, prev_score, best, moves, games, spawned, since_move,
            phase_ticks, drag_x, drag_y, can_undo, keep_going, dragging, phase)
