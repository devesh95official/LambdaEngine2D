// Camera follow and trauma shake. Following starts
// from the clamped position, so at a map edge the camera doesn't build up distance to catch up on.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <lam/math/scalar.h>
#include <lam/render/camera_math.h>

namespace lam {
namespace {

bool finite_dt(float dt) {
    return dt > 0.0f && is_finite(dt);
}

// Wrap the noise clock so it doesn't lose float precision in a long session.
constexpr int kNoisePeriod = 256;

// [-1, 1] per lattice point and channel.
float lattice(int index, std::uint32_t channel) {
    std::uint32_t h = static_cast<std::uint32_t>(index & (kNoisePeriod - 1)) * 0x9E3779B1u +
                      channel * 0x85EBCA6Bu + 0x27D4EB2Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFu) / 32767.5f - 1.0f;
}

// Smoothstep between lattice values; raw per-frame noise reads as flicker, not shake.
float noise(float time, std::uint32_t channel) {
    // Wrap first so the int cast can't overflow.
    time = std::fmod(time, static_cast<float>(kNoisePeriod));
    if (time < 0.0f) time += static_cast<float>(kNoisePeriod);
    const float whole = std::floor(time);
    const float t = time - whole;
    const int index = static_cast<int>(whole) & (kNoisePeriod - 1);
    const float a = lattice(index, channel);
    const float b = lattice(index + 1, channel);
    const float eased = t * t * (3.0f - 2.0f * t);
    return a + (b - a) * eased;
}

float shake_axis(const CameraShake &s, std::uint32_t channel) {
    if (!(s.trauma > 0.0f) || !(s.max_offset > 0.0f) || !is_finite(s.max_offset)) return 0.0f;
    const float exponent = is_finite(s.exponent) && s.exponent > 0.0f ? s.exponent : 1.0f;
    const float strength = s.max_offset * std::pow(std::min(s.trauma, 1.0f), exponent);
    const float limit = std::floor(s.max_offset);
    const float value = strength * noise(is_finite(s.time) ? s.time : 0.0f, channel);
    if (!is_finite(value)) return 0.0f;
    return std::clamp(std::round(value), -limit, limit);
}

// A map smaller than the view stays centred.
float clamp_axis(float center, float view, float lo, float size) {
    const float lo_c = lo + view * 0.5f;
    const float hi_c = lo + size - view * 0.5f;
    // Rounding can invert the bounds when size == view, and std::clamp would be UB.
    if (size < view || hi_c < lo_c) return lo + size * 0.5f;
    return std::clamp(center, lo_c, hi_c);
}

float outside(float local, float lo, float size) {
    if (local < lo) return local - lo;
    if (local > lo + size) return local - (lo + size);
    return 0.0f;
}

} // namespace

float half_life_factor(float half_life, float dt) {
    if (!(half_life > 0.0f)) return 1.0f;
    if (!(dt > 0.0f)) return 0.0f;
    return 1.0f - std::exp2(-dt / half_life);
}

Vec2 camera_lookahead_goal(Vec2 target_velocity, const CameraFollow &f) {
    if (!(f.lookahead_seconds > 0.0f) || !(f.lookahead_max > 0.0f) || !is_finite(target_velocity))
        return {};
    // Cap the length, not each axis, or diagonals would look further ahead.
    return clamp_length(target_velocity * f.lookahead_seconds, f.lookahead_max);
}

Vec2 camera_lookahead_step(Vec2 lookahead, Vec2 target_velocity, const CameraFollow &f, float dt) {
    if (!finite_dt(dt) || !is_finite(target_velocity) || !is_finite(lookahead)) return lookahead;
    const Vec2 goal = camera_lookahead_goal(target_velocity, f);
    const float k = half_life_factor(f.lookahead_half_life, dt);
    return lookahead + (goal - lookahead) * k;
}

Vec2 camera_follow_point(Vec2 camera, Vec2 aim, const CameraFollow &f, float dt) {
    if (!finite_dt(dt) || !is_finite(aim) || !is_finite(camera)) return camera;
    Vec2 goal = aim;
    if (!f.dead_zone.empty()) {
        const Vec2 local = aim - camera;
        goal = camera + Vec2{outside(local.x, f.dead_zone.x, f.dead_zone.w),
                             outside(local.y, f.dead_zone.y, f.dead_zone.h)};
    }
    const float k = half_life_factor(f.half_life, dt);
    if (k >= 1.0f) return goal;
    return camera + (goal - camera) * k;
}

Vec2 camera_follow_step(Vec2 camera, Vec2 target, Vec2 target_velocity, const CameraFollow &f,
                        float dt) {
    if (!is_finite(target) || !is_finite(target_velocity)) return camera;
    return camera_follow_point(camera, target + camera_lookahead_goal(target_velocity, f), f, dt);
}

Vec2 clamp_to_bounds(Vec2 center, Vec2 view_size_world, const Rect &bounds) {
    if (bounds.empty() || !is_finite(center) || !is_finite(view_size_world)) return center;
    if (!is_finite(bounds.x) || !is_finite(bounds.y) || !is_finite(bounds.w) ||
        !is_finite(bounds.h))
        return center;
    return {clamp_axis(center.x, view_size_world.x, bounds.x, bounds.w),
            clamp_axis(center.y, view_size_world.y, bounds.y, bounds.h)};
}

void add_trauma(CameraShake &s, float amount) {
    if (!(amount > 0.0f) || !is_finite(amount)) return;
    const float base = is_finite(s.trauma) ? std::clamp(s.trauma, 0.0f, 1.0f) : 0.0f;
    s.trauma = std::min(1.0f, base + amount);
}

void decay_trauma(CameraShake &s, float dt) {
    if (!finite_dt(dt)) return;
    const float decay = is_finite(s.decay_per_second) ? std::max(s.decay_per_second, 0.0f) : 0.0f;
    const float trauma = is_finite(s.trauma) ? s.trauma : 0.0f;
    s.trauma = std::clamp(trauma - decay * dt, 0.0f, 1.0f);
    const float freq = is_finite(s.frequency_hz) ? std::max(s.frequency_hz, 0.0f) : 0.0f;
    float t = std::fmod((is_finite(s.time) ? s.time : 0.0f) + freq * dt,
                        static_cast<float>(kNoisePeriod));
    if (!is_finite(t) || t < 0.0f) t = 0.0f;
    s.time = t;
}

Vec2 camera_shake_offset(const CameraShake &s, std::uint32_t seed) {
    const std::uint32_t base = seed * 2u;
    return {shake_axis(s, base), shake_axis(s, base + 1u)};
}

} // namespace lam
