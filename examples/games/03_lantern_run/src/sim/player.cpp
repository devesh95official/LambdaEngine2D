// The traveller: walk (TopDownMotor), dash (with a moment of protection) and knockback after a
// bite. Walking is the engine motor's job (Physics -1500): this system only writes its intent.
// Dash and knockback are fixed-speed moves the motor does not do, so they go through move_body
// (the motors' own tile + solid sweep) and the motor is told to stand still meanwhile.
#include <algorithm>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
#include <lam/physics/components.h>
#include <lam/physics/motors.h>
#include <lam/physics/queries.h>

#include "sim/controls.h"
#include "sim/rules.h"

namespace lantern {

void player_rules(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    lam::Entity e;
    Player *found = w.single_or_null<Player>(&e);
    if (found == nullptr) return;
    Player &p = *found;
    auto *motor = w.try_get<lam::TopDownMotor>(e);
    auto *velocity = w.try_get<lam::Velocity>(e);
    if (motor == nullptr || velocity == nullptr) return;
    const Tuning &t = tuning(ctx);
    const float dt = ctx.time().tick_dt;

    const Controls *controls = ctx.try_res<Controls>();
    const lam::InputFrame &in = lam::input_frame(ctx);
    const lam::Vec2 move = controls ? clamp_input(in.axis2(controls->move)) : lam::Vec2{};
    const bool dash = controls != nullptr && in.is_pressed(controls->dash);

    if (!p.alive) {
        motor->move = {};
        return;
    }
    if (p.protection > 0) --p.protection;
    if (p.dash_cooldown > 0) --p.dash_cooldown;

    const lam::Vec2 feet = w.get<lam::Transform>(e).position;
    if (p.dash_left == 0 && dash && p.dash_cooldown == 0 && lam::length_sq(p.knockback) == 0.0f) {
        p.dash_dir = lam::normalize_or(move, facing_vector(p.facing));
        p.dash_left = t.dash_ticks;
        p.dash_cooldown = t.dash_cooldown;
        ctx.emit(PlayerDashed{feet});
    }

    if (p.dash_left > 0) {
        --p.dash_left;
        motor->move = {};
        velocity->linear = {};                                  // walking restarts from rest
        p.facing = facing_for(p.dash_dir, p.facing);
        lam::move_body(w, e, p.dash_dir * (t.dash_speed * dt), motor->solid_mask);
        return;
    }
    if (lam::length_sq(p.knockback) > 0.0f) {
        motor->move = {};
        velocity->linear = {};
        const lam::BoxMove moved = lam::move_body(w, e, p.knockback * dt, motor->solid_mask);
        const float speed = lam::length(p.knockback);
        const float slower = std::max(0.0f, speed - t.knockback_friction * dt);
        p.knockback = speed > 0.0f ? p.knockback * (slower / speed) : lam::Vec2{};
        if (moved.hit_x || moved.hit_y) p.knockback = {};      // a wall stops the stagger
        return;
    }
    motor->move = move;                                         // the motor accelerates and slides
    p.facing = facing_for(move, p.facing);
}

} // namespace lantern
