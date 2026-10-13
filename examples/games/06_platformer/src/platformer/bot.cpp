// The autoplay bot: senses from the focused world, a pure decision, and an InputFrame written
// exactly like live input (axis + latched press + held bits).
#include <array>
#include <cmath>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/components.h>
#include <lam/physics/motors.h>
#include "platformer/bot.h"

namespace plat {
using namespace lam;
namespace {
constexpr int kFullJumpTicks = 18;                              // ~ jump_speed / gravity at 60 Hz
constexpr int kStuckTicks = 12;
constexpr int kResultScreenTicks = 150;                         // demo loop: read the result, then Close
}  // namespace

BotDecision bot_decide(const TileWorld &tw, const BotSenses &s, int stuck_ticks) {
    BotDecision d;
    if (!s.grounded) return d;                                  // only decide jumps from the ground
    const float front = s.feet.x + s.half.x;
    const auto solid = [&](float x, float y) { return tw.at(tw.tile_at({x, y})) == TileShape::Solid; };
    // A wall at least two tiles high just ahead (slopes are walkable, so they never count).
    for (float dx = 2; dx <= 12; dx += 5) {
        if (s.wall_ahead || (solid(front + dx, s.feet.y - 6) && solid(front + dx, s.feet.y - 18))) {
            d.jump = BotReason::Wall;
            return d;
        }
    }
    // Hazard tiles at feet height just ahead.
    for (float dx = 2; dx <= 14; dx += 4) {
        if ((tw.flags_at(tw.tile_at({front + dx, s.feet.y - 4})) & kTileHazard) != 0) {
            d.jump = BotReason::Hazard;
            return d;
        }
    }
    // A gap: nothing to stand on in the column ahead within three tiles below the feet. Cells
    // outside the map count as a gap even though TileWorld::outside_solid makes them collide.
    const IVec2 col = tw.tile_at({front + 6, s.feet.y + 1});
    bool support = false;
    for (int dy = 0; dy < 3 && !support; ++dy) {
        const IVec2 c{col.x, col.y + dy};
        support = tw.shapes.inside(c) && tw.at(c) != TileShape::Empty;
    }
    if (!support) {
        d.jump = BotReason::Gap;
        return d;
    }
    // An enemy walking at us on the same level: jump over it (or onto it: a stomp).
    for (const Vec2 e : s.enemies) {
        const float dx = e.x - s.feet.x;
        if (dx > 0 && dx < 44 && std::fabs(e.y - s.feet.y) < 16) {
            d.jump = BotReason::Enemy;
            return d;
        }
    }
    if (stuck_ticks > kStuckTicks) d.jump = BotReason::Stuck;
    return d;
}

void PlatformerBot::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    InputFrame &f = frames[0];
    f.clear_devices();                                          // the bot replaces live devices, keeps intents
    World &w = ctx.world();                                     // focused world
    const Level *lv = w.resources().try_get<Level>();
    if (lv == nullptr) {                                        // a menu world is focused: the result screen
        hold_ticks_ = 0;
        if (close_result_ && ++menu_ticks_ == kResultScreenTicks)
            f.push_intent(Intent{.kind = kMenuSelect, .a = 0}); // item 0 = "Close" -> the level restarts
        return;
    }
    menu_ticks_ = 0;
    const TileWorld *tw = w.resources().try_get<TileWorld>();
    if (tw == nullptr || !lv->started || lv->complete || lv->game_over) {
        hold_ticks_ = 0;
        return;
    }
    std::array<Vec2, 8> enemies{};
    std::size_t n = 0;
    for (auto [e, tf, patrol] : w.view<Transform, Patrol>())
        if (n < enemies.size()) enemies[n++] = tf.position;
    BotSenses s;
    bool found = false;
    for (auto [e, tf, motor, vel, col] : w.view<Transform, PlatformerMotor, Velocity, Collider>(With<Player>{})) {
        s.feet = tf.position;
        s.half = col.half;
        s.velocity = vel.linear;
        s.grounded = motor.on_ground;
        s.wall_ahead = motor.on_wall_right;
        found = true;
    }
    if (!found) return;
    s.enemies = std::span<const Vec2>(enemies.data(), n);
    stuck_ticks_ = (s.grounded && std::fabs(s.velocity.x) < 5.0f) ? stuck_ticks_ + 1 : 0;

    const BotDecision d = bot_decide(*tw, s, stuck_ticks_);
    f.set_axis(controls_.move, {d.move_x, 0});
    if (hold_ticks_ == 0 && d.jump != BotReason::None) {
        f.press(controls_.jump);
        hold_ticks_ = kFullJumpTicks;
        stuck_ticks_ = 0;
        ++jumps_;
    } else if (hold_ticks_ > 0) {
        f.hold(controls_.jump);                                 // held -> full height (no jump_cut)
    }
    if (hold_ticks_ > 0) --hold_ticks_;
}
}
