// The --bot driver. fill() runs every tick at TickBegin with the focused world and sets the same
// InputFrame bits a keyboard would.
#include <cmath>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/components.h>
#include <lam/physics/motors.h>
#include "platformer/game.h"

namespace platformer {
using namespace lam;
namespace {
constexpr int kHoldTicks = 18;                                  // hold jump to the apex: full height
constexpr int kStuckTicks = 12;                                 // grounded and not moving: try a jump

// Sets one action bit, as the input latch does. InputFrame::press/hold do the same.
void set_bit(ActionBits &bits, ButtonAction a) {
    if (a) bits[a.id >> 6] |= std::uint64_t{1} << (a.id & 63u);
}
}  // namespace

// Feet are the bottom centre of the hero's box (Transform.position in prefabs.json).
bool bot_should_jump(const TileWorld &tiles, Vec2 feet, float half_width) {
    const float front = feet.x + half_width;
    for (float dx = 2; dx <= 14; dx += 4) {                     // a wall or spikes just ahead
        const IVec2 cell = tiles.tile_at({front + dx, feet.y - 6});
        if (tiles.at(cell) == TileShape::Solid || (tiles.flags_at(cell) & kTileHazard) != 0) return true;
    }
    // A pit: nothing to stand on in the next column within three tiles down. Cells outside the map
    // count as a pit too (they collide, but the rules kill the hero down there).
    const IVec2 below = tiles.tile_at({front + 6, feet.y + 1});
    for (int dy = 0; dy < 3; ++dy) {
        const IVec2 c{below.x, below.y + dy};
        if (tiles.shapes.inside(c) && tiles.at(c) != TileShape::Empty) return false;
    }
    return true;
}

void Bot::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    World &w = ctx.world();
    const Level *lv = w.resources().try_get<Level>();
    const TileWorld *tiles = w.resources().try_get<TileWorld>();
    if (lv == nullptr || tiles == nullptr) return;              // menu or result screen
    // The frame holds live input and this tick's queued UI intents. Clear the devices only.
    InputFrame &f = frames[0];
    f.pressed = f.held = f.released = f.repeated = ActionBits{};
    f.axes = {};
    if (!lv->started || lv->won) {
        hold_ticks_ = 0;
        return;
    }
    for (auto [e, tf, motor, vel, col] : w.view<Transform, PlatformerMotor, Velocity, Collider>(With<Player>{})) {
        if (controls.move) f.axes[controls.move.id] = {1, 0};  // always run right (TODO: smarter bot)
        stuck_ticks_ = motor.on_ground && std::fabs(vel.linear.x) < 5 ? stuck_ticks_ + 1 : 0;
        const bool jump = motor.on_wall_right || stuck_ticks_ > kStuckTicks ||
                          bot_should_jump(*tiles, tf.position, col.half.x);
        if (hold_ticks_ == 0 && motor.on_ground && jump) {
            set_bit(f.pressed, controls.jump);
            set_bit(f.repeated, controls.jump);
            hold_ticks_ = kHoldTicks;
            stuck_ticks_ = 0;
        }
        if (hold_ticks_ > 0) {                                  // held: full jump (jump_cut on release)
            set_bit(f.held, controls.jump);
            --hold_ticks_;
        }
    }
}
}
