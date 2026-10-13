// Scalar helpers and the deterministic trig (det_sin, det_cos, det_atan2) used instead of libm.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include <lam/math/rng.h>
#include <lam/math/scalar.h>

using namespace lam;

namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// |a - b| measured around the circle, in double.
double angle_gap(double a, double b) {
    return std::fabs(std::remainder(a - b, 2.0 * 3.141592653589793));
}
} // namespace

static_assert(clamp(5.0f, 0.0f, 1.0f) == 1.0f);
static_assert(clamp(-5.0f, 0.0f, 1.0f) == 0.0f);
static_assert(clamp_int(7, 0, 3) == 3);
static_assert(saturate(0.25f) == 0.25f);
static_assert(lerp(2.0f, 4.0f, 0.5f) == 3.0f);
static_assert(inverse_lerp(3.0f, 3.0f, 9.0f) == 0.0f);
static_assert(sign(-2.0f) == -1.0f && sign(0.0f) == 0.0f && sign(3.0f) == 1.0f);

TEST_CASE("scalar: clamp maps NaN to the low bound and keeps in-range values") {
    CHECK(clamp(kNaN, 0.0f, 1.0f) == 0.0f);
    CHECK(clamp(kNaN, -3.0f, 5.0f) == -3.0f);
    CHECK(clamp(0.5f, 0.0f, 1.0f) == 0.5f);
    CHECK(clamp(kInf, 0.0f, 1.0f) == 1.0f);
    CHECK(clamp(-kInf, 0.0f, 1.0f) == 0.0f);
    CHECK(saturate(kNaN) == 0.0f);
    CHECK(clamp_int(-9, -2, 2) == -2);
    CHECK(clamp_int(1, -2, 2) == 1);
}

TEST_CASE("scalar: lerp, inverse_lerp and remap") {
    CHECK(lerp(10.0f, 20.0f, 0.25f) == doctest::Approx(12.5f));
    CHECK(lerp(10.0f, 20.0f, 2.0f) == doctest::Approx(30.0f)); // unclamped
    CHECK(inverse_lerp(10.0f, 20.0f, 15.0f) == doctest::Approx(0.5f));
    CHECK(remap(5.0f, 0.0f, 10.0f, 100.0f, 200.0f) == doctest::Approx(150.0f));
    CHECK(deg_to_rad(180.0f) == doctest::Approx(kPi));
    CHECK(rad_to_deg(kPi * 0.5f) == doctest::Approx(90.0f));
    CHECK(nearly_equal(1.0f, 1.0f + 1e-6f));
    CHECK_FALSE(nearly_equal(1.0f, 1.1f));
    CHECK(is_finite(1.0f));
    CHECK_FALSE(is_finite(kNaN));
    CHECK_FALSE(is_finite(kInf));
}

TEST_CASE("scalar: wrap_angle lands in (-pi, pi] and keeps the direction") {
    for (int i = -20000; i <= 20000; ++i) {
        const float x = static_cast<float>(i) * 0.0123f;
        const float r = wrap_angle(x);
        REQUIRE(r > -kPi);
        REQUIRE(r <= kPi);
        REQUIRE(angle_gap(r, x) < 1e-5);
    }
    CHECK(wrap_angle(0.0f) == 0.0f);
    CHECK(wrap_angle(1.5f * kPi) == doctest::Approx(-0.5f * kPi));
    CHECK(wrap_angle(-1.5f * kPi) == doctest::Approx(0.5f * kPi));
    CHECK(wrap_angle(-kPi) == doctest::Approx(kPi)); // -pi itself is outside the range
    CHECK(wrap_angle(kNaN) == 0.0f);
    CHECK(wrap_angle(kInf) == 0.0f);
    CHECK(wrap_angle(-kInf) == 0.0f);
}

TEST_CASE("approach and smooth_toward never overshoot") {
    CHECK(approach(0.0f, 1.0f, 0.4f) == doctest::Approx(0.4f));
    CHECK(approach(0.9f, 1.0f, 0.4f) == 1.0f);
    CHECK(approach(1.0f, -1.0f, 5.0f) == -1.0f);
    CHECK(approach(3.0f, 9.0f, 0.0f) == 3.0f);
    CHECK(approach(3.0f, 9.0f, -1.0f) == 3.0f);
    CHECK(approach(3.0f, 9.0f, kNaN) == 3.0f);
    CHECK(smooth_toward(0.0f, 8.0f, 1.0f, 1.0f) == doctest::Approx(4.0f));
    // Two half steps equal one whole step: frame-rate independent.
    const float once = smooth_toward(0.0f, 10.0f, 0.3f, 0.2f);
    const float twice = smooth_toward(smooth_toward(0.0f, 10.0f, 0.3f, 0.1f), 10.0f, 0.3f, 0.1f);
    CHECK(once == doctest::Approx(twice));
    CHECK(smooth_toward(2.0f, 8.0f, 0.0f, 0.1f) == 8.0f); // half_life <= 0 -> target
    CHECK(smooth_toward(2.0f, 8.0f, 1.0f, 0.0f) == 2.0f); // dt <= 0 -> current
    CHECK(smooth_toward(2.0f, 8.0f, 1.0f, -1.0f) == 2.0f);

    Rng rng(77);
    for (int i = 0; i < 5000; ++i) {
        const float cur = rng.range(-100.0f, 100.0f);
        const float target = rng.range(-100.0f, 100.0f);
        const float step = rng.range(0.0f, 50.0f);
        const float lo = cur < target ? cur : target;
        const float hi = cur < target ? target : cur;
        const float a = approach(cur, target, step);
        REQUIRE(a >= lo);
        REQUIRE(a <= hi);
        REQUIRE(std::fabs(a - cur) <= step + 1e-4f);
        const float s = smooth_toward(cur, target, rng.range(0.01f, 2.0f), rng.range(0.0f, 5.0f));
        REQUIRE(s >= lo);
        REQUIRE(s <= hi);
    }
}

TEST_CASE("det_sin and det_cos match std within 1e-6 over a dense sweep") {
    double worst = 0.0;
    for (int i = -400000; i <= 400000; ++i) {
        const float x = static_cast<float>(i) * 0.0000785f; // about [-31.4, 31.4]
        const double xs = static_cast<double>(x);
        const double es = std::fabs(static_cast<double>(det_sin(x)) - std::sin(xs));
        const double ec = std::fabs(static_cast<double>(det_cos(x)) - std::cos(xs));
        worst = es > worst ? es : worst;
        worst = ec > worst ? ec : worst;
    }
    CHECK(worst < 1e-6);
    for (float x : {1000.0f, -1000.0f, 12345.678f, -98765.4f, kPi, -kPi, 0.5f * kPi}) {
        CHECK(std::fabs(static_cast<double>(det_sin(x)) - std::sin(static_cast<double>(x))) < 1e-6);
        CHECK(std::fabs(static_cast<double>(det_cos(x)) - std::cos(static_cast<double>(x))) < 1e-6);
    }
    CHECK(det_sin(0.0f) == 0.0f);
    CHECK(det_cos(0.0f) == 1.0f);
    CHECK(det_sin(kNaN) == 0.0f);
    CHECK(det_cos(kNaN) == 1.0f);
    CHECK(det_sin(kInf) == 0.0f);
}

TEST_CASE("det_atan2 matches std within 1e-6 and gets every quadrant right") {
    double worst = 0.0;
    for (float radius : {1e-3f, 0.7f, 1.0f, 250.0f, 1e6f}) {
        for (int i = -50000; i <= 50000; ++i) {
            const double theta = static_cast<double>(i) * (3.141592653589793 / 50000.0);
            const auto y = static_cast<float>(radius * std::sin(theta));
            const auto x = static_cast<float>(radius * std::cos(theta));
            const double want = std::atan2(static_cast<double>(y), static_cast<double>(x));
            const double gap = angle_gap(static_cast<double>(det_atan2(y, x)), want);
            worst = gap > worst ? gap : worst;
        }
    }
    CHECK(worst < 1e-6);

    Rng rng(4);
    for (int i = 0; i < 20000; ++i) {
        const float y = rng.range(-1e4f, 1e4f);
        const float x = rng.range(-1e4f, 1e4f);
        const double want = std::atan2(static_cast<double>(y), static_cast<double>(x));
        REQUIRE(angle_gap(static_cast<double>(det_atan2(y, x)), want) < 1e-6);
    }

    // Quadrants and axes.
    CHECK(det_atan2(1.0f, 1.0f) == doctest::Approx(0.25f * kPi));
    CHECK(det_atan2(1.0f, -1.0f) == doctest::Approx(0.75f * kPi));
    CHECK(det_atan2(-1.0f, -1.0f) == doctest::Approx(-0.75f * kPi));
    CHECK(det_atan2(-1.0f, 1.0f) == doctest::Approx(-0.25f * kPi));
    CHECK(det_atan2(0.0f, 1.0f) == 0.0f);
    CHECK(det_atan2(1.0f, 0.0f) == doctest::Approx(0.5f * kPi));
    CHECK(det_atan2(-1.0f, 0.0f) == doctest::Approx(-0.5f * kPi));
    CHECK(det_atan2(0.0f, -1.0f) == doctest::Approx(kPi));
    CHECK(det_atan2(0.0f, 0.0f) == 0.0f);
    CHECK(det_atan2(kNaN, 1.0f) == 0.0f);
    CHECK(det_atan2(1.0f, kInf) == 0.0f);
    // Results stay inside [-pi, pi].
    for (int i = 0; i < 1000; ++i) {
        const float a = det_atan2(rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f));
        REQUIRE(a >= -kPi);
        REQUIRE(a <= kPi);
    }
}

TEST_CASE("det trig gives the same bits on every call") {
    for (int i = 0; i < 1000; ++i) {
        const float x = static_cast<float>(i) * 0.37f - 150.0f;
        REQUIRE(det_sin(x) == det_sin(x));
        REQUIRE(det_cos(x) == det_cos(x));
        REQUIRE(det_atan2(x, 3.0f) == det_atan2(x, 3.0f));
    }
}
