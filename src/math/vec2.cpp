#include <lam/math/vec2.h>

#include <cmath>

namespace lam {

float length(Vec2 v) { return std::sqrt(length_sq(v)); }

float distance(Vec2 a, Vec2 b) { return length(a - b); }

Vec2 normalize_or(Vec2 v, Vec2 fallback) {
    const float len = length(v);
    if (!std::isfinite(len) || len < 1e-3f) return fallback;
    return v / len;
}

Vec2 clamp_length(Vec2 v, float max_len) {
    if (!(max_len > 0.0f)) return {};
    const float sq = length_sq(v);
    if (!(sq > max_len * max_len)) return v;
    return v * (max_len / std::sqrt(sq));
}

Vec2 rotate(Vec2 v, float radians) {
    const float s = det_sin(radians);
    const float c = det_cos(radians);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

float angle_of(Vec2 v) { return det_atan2(v.y, v.x); }

Vec2 from_angle(float radians, float len) { return {det_cos(radians) * len, det_sin(radians) * len}; }

Vec2 floor(Vec2 v) { return {std::floor(v.x), std::floor(v.y)}; }

Vec2 round(Vec2 v) { return {std::floor(v.x + 0.5f), std::floor(v.y + 0.5f)}; }

bool is_finite(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

IVec2 floor_to_ivec(Vec2 v) {
    return {static_cast<int>(std::floor(v.x)), static_cast<int>(std::floor(v.y))};
}

Vec2 approach(Vec2 current, Vec2 target, float max_step) {
    if (!(max_step > 0.0f)) return current;
    const Vec2 d = target - current;
    const float len = length(d);
    if (len <= max_step) return target;
    return current + d * (max_step / len);
}

} // namespace lam
