#include <lam/math/scalar.h>

#include <cmath>

namespace lam {
namespace {

constexpr double kPiD = 3.141592653589793238462643383279502884;
constexpr double kTauD = 6.283185307179586476925286766559005768;
constexpr double kHalfPiD = 1.570796326794896619231321691639751442;

// Reduces x into [-pi, pi] with IEEE-exact operations only (division, nearbyint, multiply-add
// kept separate by -ffp-contract=off), so the result has the same bits on every build.
double reduce(double x) {
    const double k = std::nearbyint(x / kTauD);
    return x - k * kTauD;
}

// sin on [-pi/2, pi/2]: Taylor series to x^17 in double (error far below float precision).
double sin_core(double x) {
    const double x2 = x * x;
    double p = 1.0 / 355687428096000.0;          // 1/17!
    p = p * x2 - 1.0 / 1307674368000.0;          // 1/15!
    p = p * x2 + 1.0 / 6227020800.0;             // 1/13!
    p = p * x2 - 1.0 / 39916800.0;               // 1/11!
    p = p * x2 + 1.0 / 362880.0;                 // 1/9!
    p = p * x2 - 1.0 / 5040.0;                   // 1/7!
    p = p * x2 + 1.0 / 120.0;                    // 1/5!
    p = p * x2 - 1.0 / 6.0;                      // 1/3!
    p = p * x2 + 1.0;
    return p * x;
}

double det_sin_d(double x) {
    double r = reduce(x);
    if (r > kHalfPiD) r = kPiD - r;
    else if (r < -kHalfPiD) r = -kPiD - r;
    return sin_core(r);
}

// atan on |x| <= ~0.2: Taylor series to x^21.
double atan_core(double x) {
    const double x2 = x * x;
    double p = 1.0 / 21.0;
    p = 1.0 / 19.0 - p * x2;
    p = 1.0 / 17.0 - p * x2;
    p = 1.0 / 15.0 - p * x2;
    p = 1.0 / 13.0 - p * x2;
    p = 1.0 / 11.0 - p * x2;
    p = 1.0 / 9.0 - p * x2;
    p = 1.0 / 7.0 - p * x2;
    p = 1.0 / 5.0 - p * x2;
    p = 1.0 / 3.0 - p * x2;
    p = 1.0 - p * x2;
    return p * x;
}

// atan for any finite x: reflect to |x| <= 1, then halve the angle twice with
// atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) (sqrt is correctly rounded, so deterministic).
double det_atan_d(double x) {
    const bool negative = x < 0.0;
    double a = negative ? -x : x;
    bool inverted = false;
    if (a > 1.0) {
        a = 1.0 / a;
        inverted = true;
    }
    a = a / (1.0 + std::sqrt(1.0 + a * a));
    a = a / (1.0 + std::sqrt(1.0 + a * a));
    double r = 4.0 * atan_core(a);
    if (inverted) r = kHalfPiD - r;
    return negative ? -r : r;
}

} // namespace

float wrap_angle(float radians) {
    if (!std::isfinite(radians)) return 0.0f;
    double r = reduce(static_cast<double>(radians));
    if (r <= -kPiD) r += kTauD;
    float f = static_cast<float>(r);
    if (f <= -kPi) f = kPi;
    return f;
}

bool is_finite(float v) { return std::isfinite(v); }

bool nearly_equal(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

float approach(float current, float target, float max_step) {
    if (!(max_step > 0.0f)) return current;
    if (current < target) return current + max_step < target ? current + max_step : target;
    return current - max_step > target ? current - max_step : target;
}

float smooth_toward(float current, float target, float half_life, float dt) {
    if (!(half_life > 0.0f)) return target;
    if (!(dt > 0.0f)) return current;
    const float keep = std::exp2(-dt / half_life);
    return target + (current - target) * keep;
}

float det_sin(float radians) {
    if (!std::isfinite(radians)) return 0.0f;
    return static_cast<float>(det_sin_d(static_cast<double>(radians)));
}

float det_cos(float radians) {
    if (!std::isfinite(radians)) return 1.0f;
    return static_cast<float>(det_sin_d(static_cast<double>(radians) + kHalfPiD));
}

float det_atan2(float y, float x) {
    if (!std::isfinite(x) || !std::isfinite(y)) return 0.0f;
    const double dy = y, dx = x;
    if (dx == 0.0) {
        if (dy > 0.0) return static_cast<float>(kHalfPiD);
        if (dy < 0.0) return static_cast<float>(-kHalfPiD);
        return 0.0f;
    }
    double r = det_atan_d(dy / dx);
    if (dx < 0.0) r += dy < 0.0 ? -kPiD : kPiD;
    return static_cast<float>(r);
}

} // namespace lam
