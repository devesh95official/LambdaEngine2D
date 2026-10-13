#pragma once
// Tetris rules. The whole run is one snapshotted resource, Board.
#include <array>
#include <cstdint>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
#include <lam/math/vec2.h>
#include <lam/particles/particles.h>

namespace tetris {

inline constexpr int kW = 10, kH = 20;
inline constexpr int kFlashTicks = 18;                         // line-clear flash before rows fall
inline constexpr int kLockTicks = 30;                          // lock delay once grounded
inline constexpr int kMaxLockResets = 15;

// Piece kinds 1..7: I O T S Z J L (0 = empty cell).
enum Kind : std::uint8_t { kNone = 0, kI, kO, kT, kS, kZ, kJ, kL };

struct Board {
    std::array<std::uint8_t, kW * kH> cells{};                  // row-major, y down
    std::array<std::uint8_t, 7> bag{};                          // 7-bag, drawn from the front
    std::array<std::uint8_t, 5> next{};                         // preview queue
    std::uint8_t bag_left = 0;
    std::uint8_t kind = kNone, rot = 0;                         // active piece (kNone: none)
    std::int32_t x = 0, y = 0;
    std::uint8_t hold = kNone;
    bool can_hold = true;
    std::int32_t fall_ticks = 0, lock_ticks = 0, lock_resets = 0;
    std::uint32_t clear_mask = 0;                               // rows flashing (bit y)
    std::int32_t flash = 0;
    std::int32_t score = 0, lines = 0, level = 1;
    std::uint32_t pieces = 0;                                   // pieces spawned (bot plan key)
    std::int32_t tetrises = 0;
    bool over = false;
};

// Action ids (App resource).
struct Controls {
    lam::ButtonAction left, right, soft, drop, rot_cw, rot_ccw, hold, confirm, pause;
};
// Particle configs, filled by main.cpp. Missing in tests.
struct Fx { lam::AssetId<lam::EmitterConfig> spark, dust; };

// Board position in world pixels, shared by drawing and particles.
inline constexpr float kCell = 12;
inline constexpr lam::Vec2 kOrigin{180, 18};

// The four cells of `kind` in rotation `rot`, relative to the piece origin.
std::array<lam::IVec2, 4> cells_of(int kind, int rot);
bool fits(const Board &b, int kind, int rot, int x, int y);
int drop_distance(const Board &b);
int gravity_ticks(int level);

void start_run(lam::World &w);                                  // adds Board and the first piece

class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "tetris.rules"; }
    void build(lam::App &app) override;
};

} // namespace tetris

LAM_REFLECT_RESOURCE(tetris::Board, cells, bag, next, bag_left, kind, rot, x, y, hold, can_hold, fall_ticks,
                     lock_ticks, lock_resets, clear_mask, flash, score, lines, level, pieces, tetrises, over)
