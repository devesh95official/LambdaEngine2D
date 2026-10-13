#pragma once
// Autoplay. PlatformerBot writes the same InputFrame a keyboard would. bot_decide is a pure function
// of the TileWorld and a few senses, so it can be tested without physics.
#include <cstdint>
#include <span>
#include <lam/core/id.h>
#include <lam/input/driver.h>
#include <lam/math/vec2.h>
#include <lam/physics/tile_world.h>
#include "platformer/components.h"

namespace plat {
struct BotSenses {
    lam::Vec2 feet;                                             // hero Transform.position (bottom centre)
    lam::Vec2 half{5, 7};                                       // hero Collider half extents
    lam::Vec2 velocity;
    bool grounded = false;
    bool wall_ahead = false;                                    // motor.on_wall_right
    std::span<const lam::Vec2> enemies;                         // enemy feet positions
};
enum class BotReason : std::uint8_t { None, Wall, Gap, Hazard, Enemy, Stuck };
struct BotDecision {
    float move_x = 1;                                           // always run right
    BotReason jump = BotReason::None;                           // != None: start a jump now
};
// Run right; jump at walls (2+ tiles high), gaps, hazard tiles and enemies just ahead.
// `stuck_ticks`: ticks the hero has been grounded without moving.
BotDecision bot_decide(const lam::TileWorld &tw, const BotSenses &s, int stuck_ticks = 0);

// The engine menus' select intent (lam/ui/menu_scenes.h kMenuSelectIntent; a = item index). Spelled
// here so the sim library does not link lam_ui for one id.
inline constexpr lam::StrId kMenuSelect = lam::str_id("menu.select");

class PlatformerBot final : public lam::InputDriver {
public:
    void set_controls(const Controls &c) { controls_ = c; }     // before run(); ids from ActionMap
    // Demo loop (windowed --bot): when a menu world (the result screen) is focused, select its first
    // item ("Close") after a pause, so LevelScene::resumed restarts the level.
    void set_close_result(bool on) { close_result_ = on; }
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
    std::uint32_t jumps() const { return jumps_; }
private:
    Controls controls_{};
    int hold_ticks_ = 0;                                        // jump held for a full-height jump
    int stuck_ticks_ = 0;
    int menu_ticks_ = 0;
    bool close_result_ = false;
    std::uint32_t jumps_ = 0;
};
}
