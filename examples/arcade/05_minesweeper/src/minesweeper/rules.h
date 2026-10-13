#pragma once
// Minesweeper rules. The whole game is one snapshotted resource, Mine.
#include <array>
#include <cstdint>
#include <span>
#include <lam/core/app.h>
#include <lam/ecs/component.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/math/rect.h>
#include <lam/particles/particles.h>

namespace ms {

inline constexpr int kMaxCells = 16 * 16;
inline constexpr lam::IVec2 kVirtual{400, 300};
inline constexpr float kHudH = 34;
inline constexpr std::int32_t kTitle = 0, kPlay = 1, kOver = 2;               // Mine::phase
inline constexpr std::uint8_t kCount = 0x0F, kMine = 0x10, kOpen = 0x20, kFlag = 0x40;   // cell bits

struct Level { int w, h, mines, cell; const char *name; };
inline constexpr std::array<Level, 2> kLevels{{{9, 9, 10, 24, "BEGINNER"}, {16, 16, 40, 16, "INTERMEDIATE"}}};

struct Mine {                                                   // snapshotted World resource
    std::int32_t phase = kTitle, phase_ticks = 0, level = 0, w = 9, h = 9, mines = 10;
    std::array<std::uint8_t, kMaxCells> cells{};
    bool placed = false, won = false;
    std::int32_t opened = 0, flags = 0, ticks = 0, score = 0, best = 0;
    std::int32_t games = 0, wins = 0, cx = 4, cy = 4, boom = -1;
};

struct Controls {                                               // App resource
    lam::ButtonAction reveal, flag, chord, up, down, left, right, confirm, restart, back, one, two;
};
struct Fx { lam::AssetId<lam::EmitterConfig> spark, boom, confetti; };   // App resource, may be missing

lam::Rect board_rect(const Mine &m);                           // in screen pixels
lam::Rect title_button(int level);
lam::Vec2 cell_center(const Mine &m, int cell);
int cell_at(const Mine &m, lam::Vec2 p);                       // -1 outside the board
inline int seconds(const Mine &m) { return m.ticks / 60; }

class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "minesweeper.rules"; }
    void build(lam::App &app) override;
};
void start_world(lam::World &w);                               // adds and snapshots Mine

// Bot: single-cell and subset deductions, else the lowest-risk guess.
struct Move { int kind = 0; int cell = -1; };                   // 0 none, 1 reveal, 2 flag, 3 chord
Move bot_decide(const Mine &m);
class Bot final : public lam::InputDriver {
public:
    Controls c;
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
};

} // namespace ms

LAM_REFLECT_RESOURCE(ms::Mine, phase, phase_ticks, level, w, h, mines, cells, placed, won, opened, flags, ticks,
                     score, best, games, wins, cx, cy, boom)
