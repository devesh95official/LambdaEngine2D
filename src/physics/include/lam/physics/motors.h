#pragma once
// Platformer and top-down controllers. Need Transform, a Box Collider and Velocity.
// Intent fields are never cleared, so write them every tick; to stop a body, zero its intent and
// its Velocity.
#include <array>
#include <cstdint>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/vec2.h>
#include <lam/physics/components.h>
namespace lam {
struct PlatformerMotor {
    float gravity = 900;
    float max_fall = 400;
    float run_speed = 110;
    float accel = 900;
    float decel = 1200;
    float air_control = 0.7f;
    float jump_speed = 280;
    float jump_cut = 0.5f;                                      // velocity kept when jump is released early
    float climb_speed = 60;
    float snap_down = 4;                                        // ground snap down slopes (px)
    std::uint8_t coyote_ticks = 6;
    std::uint8_t buffer_ticks = 6;
    CollisionMask solid_mask = 1;                               // blocking Box colliders; tiles always block
    // Intent
    float move_x = 0;                                           // -1..1
    bool jump_pressed = false;                                  // buffered for buffer_ticks
    bool jump_held = false;                                     // letting go early cuts the jump
    bool drop = false;                                          // fall through one-way platforms
    float climb_y = 0;                                          // -1 up; non-zero grabs a ladder
    // State
    bool on_ground = false, on_wall_left = false, on_wall_right = false, on_ceiling = false;
    bool on_ladder = false, on_slope = false;
    Vec2 ground_normal{0, -1};
    std::uint8_t coyote_left = 0, buffer_left = 0;
    Entity riding;                                              // moving platform under the body
};
struct TopDownMotor {
    float max_speed = 90;
    float accel = 900;
    float decel = 1200;
    float corner_assist = 5;                                    // px, straight moves only
    CollisionMask solid_mask = 1;                               // one-way colliders don't block
    Vec2 move;                                                  // intent, |move| <= 1
    IVec2 wall;                                                 // last blocked axes (-1/0/+1)
};
struct MovingPlatform { Vec2 last_delta; bool carry_riders = true; };
// Moves a kinematic body along `points` and writes its Velocity.
struct PathFollower {
    std::array<Vec2, 8> points{};                               // first `count` are used
    std::uint8_t count = 0;
    std::uint8_t next = 1;
    float speed = 30;                                           // px/s
    bool ping_pong = true;
    bool forward = true;
};
Vec2 step_velocity(Vec2 v, Vec2 input, float max_speed, float accel, float decel, float dt);
}
LAM_REFLECT(lam::PlatformerMotor, gravity, max_fall, run_speed, accel, decel, air_control, jump_speed, jump_cut,
            climb_speed, snap_down, coyote_ticks, buffer_ticks, solid_mask, move_x, jump_pressed, jump_held,
            drop, climb_y, on_ground, on_wall_left, on_wall_right, on_ceiling, on_ladder, on_slope,
            ground_normal, coyote_left, buffer_left, riding)
LAM_REFLECT(lam::TopDownMotor, max_speed, accel, decel, corner_assist, solid_mask, move, wall)
LAM_REFLECT(lam::MovingPlatform, last_delta, carry_riders)
LAM_REFLECT(lam::PathFollower, points, count, next, speed, ping_pong, forward)
