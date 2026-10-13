#pragma once
// 2D vectors in world pixels (+x right, +y down). rotate/from_angle/angle_of use det_*.
#include <lam/math/scalar.h>
namespace lam {
// A float 2D vector or point.
struct Vec2 {
    float x = 0.0f, y = 0.0f;
    constexpr bool operator==(const Vec2 &) const = default;
};
// An integer 2D vector: grid cells, pixel sizes.
struct IVec2 {
    int x = 0, y = 0;
    constexpr bool operator==(const IVec2 &) const = default;
};
constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
constexpr Vec2 operator-(Vec2 v) { return {-v.x, -v.y}; }
constexpr Vec2 operator*(Vec2 v, float s) { return {v.x * s, v.y * s}; }
constexpr Vec2 operator*(float s, Vec2 v) { return {v.x * s, v.y * s}; }
constexpr Vec2 operator/(Vec2 v, float s) { return {v.x / s, v.y / s}; }
// Component-wise product.
constexpr Vec2 operator*(Vec2 a, Vec2 b) { return {a.x * b.x, a.y * b.y}; }
constexpr Vec2 &operator+=(Vec2 &a, Vec2 b) { return a = a + b; }
constexpr Vec2 &operator-=(Vec2 &a, Vec2 b) { return a = a - b; }
constexpr Vec2 &operator*=(Vec2 &a, float s) { return a = a * s; }
constexpr IVec2 operator+(IVec2 a, IVec2 b) { return {a.x + b.x, a.y + b.y}; }
constexpr IVec2 operator-(IVec2 a, IVec2 b) { return {a.x - b.x, a.y - b.y}; }
constexpr IVec2 operator-(IVec2 v) { return {-v.x, -v.y}; }
constexpr IVec2 operator*(IVec2 a, int s) { return {a.x * s, a.y * s}; }
constexpr float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
// z of the 3D cross product: > 0 when b is clockwise from a on screen (+y down).
constexpr float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
constexpr float length_sq(Vec2 v) { return dot(v, v); }
float length(Vec2 v);
float distance(Vec2 a, Vec2 b);
constexpr float distance_sq(Vec2 a, Vec2 b) { return length_sq(a - b); }
// Unit vector; |v| < 1e-3 or non-finite -> fallback.
Vec2 normalize_or(Vec2 v, Vec2 fallback = {});
// v shortened to at most max_len (direction kept).
Vec2 clamp_length(Vec2 v, float max_len);
constexpr Vec2 lerp(Vec2 a, Vec2 b, float t) { return {lerp(a.x, b.x, t), lerp(a.y, b.y, t)}; }
// (-y, x).
constexpr Vec2 perp(Vec2 v) { return {-v.y, v.x}; }
Vec2 rotate(Vec2 v, float radians);
// det_atan2(y, x).
float angle_of(Vec2 v);
Vec2 from_angle(float radians, float len = 1.0f);
constexpr Vec2 min(Vec2 a, Vec2 b) { return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y}; }
constexpr Vec2 max(Vec2 a, Vec2 b) { return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y}; }
Vec2 floor(Vec2 v);
// floor(v + 0.5): pixel snap.
Vec2 round(Vec2 v);
bool is_finite(Vec2 v);
constexpr Vec2 to_vec2(IVec2 v) { return {static_cast<float>(v.x), static_cast<float>(v.y)}; }
IVec2 floor_to_ivec(Vec2 v);
// Moves current toward target along the straight line by at most max_step; no overshoot.
Vec2 approach(Vec2 current, Vec2 target, float max_step);
constexpr int manhattan(IVec2 a, IVec2 b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const int dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx + dy;
}
constexpr int chebyshev(IVec2 a, IVec2 b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const int dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}
}
