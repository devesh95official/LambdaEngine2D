#pragma once
// Frogger rules and bot. Positions are integer milli-pixels so replays match exactly.
#include <cstdint>
#include <span>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/particles/components.h>

namespace frogger {

// 13x13 cells of 16 px under a 16 px HUD strip, on a 208x240 screen.
// Rows: 0 homes, 1-5 river, 6 median, 7-11 road, 12 start.
inline constexpr int kCell = 16;
inline constexpr int kCols = 13;
inline constexpr int kRows = 13;
inline constexpr int kFieldY = 16;
inline constexpr int kW = kCols * kCell;                        // 208
inline constexpr int kVH = 240;
inline constexpr int kMargin = 100;                             // movers wrap this far off screen
inline constexpr int kMilli = 1000;
inline constexpr int kHopTicks = 7;
inline constexpr int kFrogTicks = 1800;                         // 30 s per frog
inline constexpr int kHomes = 5;
inline constexpr int kStartRow = 12;
inline constexpr int kStartCx = (kCols / 2) * kCell + kCell / 2;   // px

enum MoverKind : std::uint8_t { kCar = 0, kTruck = 1, kDozer = 2, kRacer = 3, kLog = 4, kTurtle = 5 };
enum State : std::uint8_t { kTitle = 0, kPlay = 1, kDying = 2, kOver = 3 };

struct Mover {                                                  // a car, truck, log or turtle group
    std::int32_t row = 0;
    std::int32_t x = 0;                                         // left edge, milli-px
    std::int32_t w = 16;                                        // px
    std::int32_t base_speed = 0;                                // milli-px per tick at level 1
    std::uint8_t kind = kCar;
    bool diver = false;                                         // turtles that dive
};

struct Run {
    std::uint8_t state = kTitle;
    std::int32_t score = 0, best = 0;
    std::int32_t lives = 3, level = 1;
    std::uint8_t homes = 0;                                     // bit k = slot k filled
    std::int32_t row = kStartRow, cx = kStartCx * kMilli;      // frog centre (milli-px)
    std::int32_t from_row = kStartRow, from_cx = kStartCx * kMilli;
    std::int32_t best_row = kStartRow;                          // furthest row this frog reached
    std::int32_t timer = kFrogTicks;
    std::int32_t hop = 0;                                       // ticks left in the hop
    std::uint8_t facing = 0;                                    // 0 up 1 right 2 down 3 left
    std::uint8_t death = 0;                                     // 1 road, 2 water, 3 time/edge
    std::int32_t state_timer = 0;
    std::uint32_t clock = 0;                                    // ticks since the run started
    std::uint32_t deaths = 0, homed = 0, hops = 0;
};

struct Controls { lam::ButtonAction up, down, left, right, start, click; };
struct Fx { lam::AssetId<lam::EmitterConfig> splash, squash, sparkle; };

// Mover speed at a given level, milli-px per tick.
std::int32_t speed_of(const Mover &m, std::int32_t level);
// Wrapped x of a mover `t` ticks from now, milli-px.
std::int32_t x_at(const Mover &m, std::int32_t level, std::int32_t t);
// 0 surfaced, 1 sinking or rising (still rideable), 2 under.
int turtle_phase(const Mover &m, std::uint32_t clock);
inline constexpr int slot_cx(int k) { return k * 3 * kCell + kCell / 2; }   // px
// True if a frog at (row, cx) survives the next `ticks` ticks, riding whatever is under it.
bool safe_for(lam::World &w, const Run &run, int row, std::int32_t cx, int ticks);

class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "frogger.rules"; }
    void build(lam::App &app) override;
};
// Adds Run and spawns the lanes.
void start_world(lam::World &w);

class Bot final : public lam::InputDriver {
public:
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
};
} // namespace frogger

LAM_REFLECT(frogger::Mover, row, x, w, base_speed, kind, diver)
LAM_REFLECT(frogger::Run, state, score, best, lives, level, homes, row, cx, from_row, from_cx, best_row,
            timer, hop, facing, death, state_timer, clock, deaths, homed, hops)
