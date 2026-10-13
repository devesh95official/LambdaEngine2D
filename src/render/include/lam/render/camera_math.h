#pragma once
// Camera maths. Smoothing is exponential so it looks the same at any frame rate. Non-finite
// input leaves the camera alone, since one NaN would stick in the smoothed position forever.
#include <cstdint>
#include <lam/ecs/component.h>
#include <lam/math/rect.h>
#include <lam/render/camera.h>
namespace lam {
// Follow with unsmoothed look-ahead. The systems use the smoothed version below.
Vec2 camera_follow_step(Vec2 camera, Vec2 target, Vec2 target_velocity, const CameraFollow &f, float dt);
// Empty bounds leave `center` unchanged; an axis too small for the view centres on the bounds.
Vec2 clamp_to_bounds(Vec2 center, Vec2 view_size_world, const Rect &bounds);
void add_trauma(CameraShake &s, float amount);                  // clamped to [0, 1]
// Also advances the noise clock.
void decay_trauma(CameraShake &s, float dt);
// Whole world pixels. Each axis uses its own hashed value noise, so it's deterministic.
Vec2 camera_shake_offset(const CameraShake &s, std::uint32_t seed);

// Added by camera_follow. Snapshotted so undo keeps the glide, but never saved. Remove it to
// force a snap.
struct CameraFollowState {
    Vec2 lookahead;
    Vec2 last_target;
    bool tracking = false;                                      // false: snap next frame
};
// velocity * lookahead_seconds, length capped at lookahead_max.
Vec2 camera_lookahead_goal(Vec2 target_velocity, const CameraFollow &f);
// Smoothed so turning around swings the view across instead of jumping.
Vec2 camera_lookahead_step(Vec2 lookahead, Vec2 target_velocity, const CameraFollow &f, float dt);
// With a dead zone the camera only chases the part of `aim` outside it. Never overshoots.
Vec2 camera_follow_point(Vec2 camera, Vec2 aim, const CameraFollow &f, float dt);
// 1 - 2^(-dt / half_life), or 1 when half_life <= 0.
float half_life_factor(float half_life, float dt);

// Lets tick systems shake the camera without touching CameraShake:
//   ctx.emit(ShakeAt{.trauma = 0.4f});
// Adds to every CameraShake in the emitting world. Presentation only.
struct ShakeAt { float trauma = 0.3f; };
}
