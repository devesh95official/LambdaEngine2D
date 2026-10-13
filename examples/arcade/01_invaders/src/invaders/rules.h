#pragma once
// Invaders rules. The whole run is one snapshotted resource, Run.
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/ecs/world.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/particles/components.h>

namespace inv {

inline constexpr int kW = 320, kH = 240;                        // screen and world pixels match
inline constexpr int kCols = 11, kRows = 5, kCount = kCols * kRows;
inline constexpr int kCellW = 16, kCellH = 14, kInvW = 12, kInvH = 8;
inline constexpr int kPlayerY = 212, kPlayerW = 13, kPlayerH = 8;   // cannon top; x is its centre
inline constexpr int kGroundY = 224;
inline constexpr int kShieldY = 180, kShieldCW = 11, kShieldCH = 8, kShieldCell = 2, kShields = 4;
inline constexpr int kShieldCells = kShieldCW * kShieldCH;
inline constexpr int kUfoY = 20, kUfoW = 16, kUfoH = 7;
inline constexpr int kMaxBombs = 6;

inline int shield_x(int i) { return 80 * i + 40 - kShieldCW * kShieldCell / 2; }
inline int inv_x(int fx, int col) { return fx + col * kCellW; }
inline int inv_y(int fy, int row) { return fy + row * kCellH; }
inline int row_points(int row) { return row == 0 ? 30 : row < 3 ? 20 : 10; }

struct Shot {
    std::int32_t x = 0, y = 0;                                  // top-centre
    std::int32_t kind = 0;                                      // bombs: 0 zigzag, 1 plunger
    bool active = false;
};

struct Run {
    std::array<std::uint8_t, kCount> alive{};
    std::array<std::uint8_t, kShields * kShieldCells> shield{};
    std::array<Shot, kMaxBombs> bombs{};
    Shot shot;
    std::int32_t fx = 0, fy = 0, dir = 1, march_timer = 0, anim = 0, alive_count = 0;
    bool drop_next = false;
    std::int32_t player_x = kW / 2, dead_ticks = 0, bomb_timer = 60;
    std::int32_t ufo_x = 0, ufo_dir = 1, ufo_timer = 900, popup_ticks = 0, popup_x = 0, popup_value = 0;
    bool ufo_active = false;
    std::int32_t score = 0, lives = 3, wave = 1, wave_pause = 0, kills = 0;
    bool over = false;
};

// Action ids (App resource).
struct Controls {
    lam::ButtonAction left, right, fire, confirm, pause;
};

// Particle configs, filled by main.cpp. No bursts if it's missing.
enum Fx : std::uint8_t { kFxPink, kFxCyan, kFxGreen, kFxPlayer, kFxUfo, kFxChip, kFxCount };
struct FxIds {
    std::array<lam::AssetId<lam::EmitterConfig>, kFxCount> ids{};
};

bool shield_solid(int s, int cx, int cy);                       // the shape before damage
void start_run(lam::World &w);                                  // adds Run and wave 1
void step(lam::Ctx &ctx);                                       // the whole game tick

class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "invaders.rules"; }
    void build(lam::App &app) override;
};

// --bot: bot_decide picks the move, Bot presses it.
struct BotChoice {
    int move = 0;                                               // -1, 0, +1
    bool fire = false;
};
BotChoice bot_decide(const Run &r);

class Bot final : public lam::InputDriver {
public:
    Controls controls;
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;

private:
    std::uint32_t ticks_ = 0;
};

} // namespace inv

LAM_REFLECT_STRUCT(inv::Shot, x, y, kind, active)
LAM_REFLECT_RESOURCE(inv::Run, alive, shield, bombs, shot, fx, fy, dir, march_timer, anim, alive_count, drop_next,
                     player_x, dead_ticks, bomb_timer, ufo_x, ufo_dir, ufo_timer, popup_ticks, popup_x, popup_value,
                     ufo_active, score, lives, wave, wave_pause, kills, over)
