#pragma once
// Scalar helpers shared by every module. det_* are polynomial approximations: simulation code
// uses them (directly or through vec2/rng helpers) so results do not depend on libm.
namespace lam {
inline constexpr float kPi = 3.14159265358979f;
inline constexpr float kTau = 6.28318530717959f;

// Clamps v to [lo, hi]. NaN -> lo.
constexpr float clamp(float v, float lo, float hi) {
    if (!(v >= lo)) return lo;
    return v > hi ? hi : v;
}
// Clamps an int to [lo, hi].
constexpr int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
// clamp(v, 0, 1).
constexpr float saturate(float v) { return clamp(v, 0.0f, 1.0f); }
// a + (b - a) * t, unclamped.
constexpr float lerp(float a, float b, float t) { return a + (b - a) * t; }
// Where v lies between a and b (0 at a, 1 at b). a == b -> 0.
constexpr float inverse_lerp(float a, float b, float v) { return a == b ? 0.0f : (v - a) / (b - a); }
// Maps v from [a0, a1] onto [b0, b1], unclamped.
constexpr float remap(float v, float a0, float a1, float b0, float b1) {
    return lerp(b0, b1, inverse_lerp(a0, a1, v));
}
// -1, 0 or 1.
constexpr float sign(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }
constexpr float deg_to_rad(float d) { return d * (kPi / 180.0f); }
constexpr float rad_to_deg(float r) { return r * (180.0f / kPi); }
// Wraps an angle into (-pi, pi]. Non-finite -> 0.
float wrap_angle(float radians);
bool is_finite(float v);
// |a - b| <= eps.
bool nearly_equal(float a, float b, float eps = 1e-5f);
// Moves current toward target by at most max_step; never overshoots. max_step <= 0 -> current.
float approach(float current, float target, float max_step);
// Frame-rate independent exponential smoothing: after half_life seconds the gap halves.
// half_life <= 0 -> target; dt <= 0 -> current.
float smooth_toward(float current, float target, float half_life, float dt);
// Deterministic sine/cosine/atan2: |err| < 1e-6, the same bits on every build of a platform.
float det_sin(float radians);
float det_cos(float radians);
// det_atan2(0, 0) == 0.
float det_atan2(float y, float x);
}
