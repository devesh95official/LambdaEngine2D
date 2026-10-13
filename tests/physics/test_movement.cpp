// step_velocity and TopDownMotor's walled step (a private rule from the detail header).
#include <algorithm>
#include <cmath>
#include <limits>

#include <lam/physics/motors.h>
#include <lam/test/support.h>

#include "detail/physics_world.h"

using lam::IVec2;
using lam::Vec2;
using lam::detail::step_velocity_walled;

namespace {

constexpr float kTickSeconds = 1.0f / 60.0f;

// A typical tuning.
struct Preset {
    float max_speed = 90.0f;
    float acceleration = 900.0f;
    float deceleration = 1200.0f;
};

Vec2 step(Vec2 v, float ix, float iy, const Preset &c = {}) {
    return lam::step_velocity(v, Vec2{ix, iy}, c.max_speed, c.acceleration, c.deceleration, kTickSeconds);
}

Vec2 walled(Vec2 v, float ix, float iy, IVec2 wall, const Preset &c = {}) {
    return step_velocity_walled(v, Vec2{ix, iy}, c.max_speed, c.acceleration, c.deceleration, kTickSeconds, wall);
}

float speed(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

} // namespace

TEST_CASE("movement: a held direction accelerates to exactly max speed and stays there") {
    const Preset config;
    Vec2 v;
    int ticks = 0;
    float last = 0.0f;
    while (v.x != config.max_speed && ticks < 1000) {
        v = step(v, 1.0f, 0.0f);
        CHECK(v.x > last);
        CHECK(v.x <= config.max_speed);
        CHECK(v.y == 0.0f);
        last = v.x;
        ++ticks;
    }
    CHECK(ticks == 6);                                           // 90 / 900 = 0.1 s = 6 ticks
    for (int i = 0; i < 100; ++i) {
        v = step(v, 1.0f, 0.0f);
        CHECK(v.x == config.max_speed);
    }
}

TEST_CASE("movement: releasing the stick decelerates to an exact stop") {
    Vec2 v{-63.0f, 51.0f};
    int ticks = 0;
    float last = speed(v);
    while ((v.x != 0.0f || v.y != 0.0f) && ticks < 1000) {
        v = step(v, 0.0f, 0.0f);
        CHECK(speed(v) < last);
        last = speed(v);
        ++ticks;
    }
    CHECK(v.x == 0.0f);
    CHECK(v.y == 0.0f);
    CHECK(ticks == 5);                                           // 81.06 px/s at 20 px/s per tick
    v = step(v, 0.0f, 0.0f);
    CHECK(v == Vec2{});
}

TEST_CASE("movement: deceleration and acceleration are separate numbers") {
    Preset config;
    config.acceleration = 60.0f;
    config.deceleration = 6000.0f;
    Vec2 v = step(Vec2{}, 1.0f, 0.0f, config);
    CHECK(v.x == doctest::Approx(1.0f));
    v = step(Vec2{90.0f, 0.0f}, 0.0f, 0.0f, config);
    CHECK(v.x == 0.0f);
}

TEST_CASE("movement: an analog half tilt settles at half of max speed") {
    Vec2 v;
    for (int i = 0; i < 60; ++i) v = step(v, 0.5f, 0.0f);
    CHECK(v.x == 45.0f);
    CHECK(v.y == 0.0f);
    v = Vec2{};
    for (int i = 0; i < 60; ++i) v = step(v, -0.3f, 0.4f);
    CHECK(v.x == -0.3f * 90.0f);
    CHECK(v.y == 0.4f * 90.0f);
    // Easing the stick back from full to half slows down under acceleration and settles.
    v = Vec2{90.0f, 0.0f};
    v = step(v, 0.5f, 0.0f);
    CHECK(v.x == doctest::Approx(75.0f));
    for (int i = 0; i < 60; ++i) v = step(v, 0.5f, 0.0f);
    CHECK(v.x == 45.0f);
}

TEST_CASE("movement: a diagonal is never faster than a straight line, at any time") {
    const float diagonal = std::sqrt(0.5f);
    Vec2 v;
    for (int i = 0; i < 30; ++i) {
        v = step(v, diagonal, diagonal);
        CHECK(speed(v) <= 90.0f * 1.00001f);
    }
    CHECK(speed(v) == doctest::Approx(90.0f));
    for (int i = 0; i < 30; ++i) {
        v = step(v, -1.0f, 0.0f);
        CHECK(speed(v) <= 90.0f * 1.00001f);
    }
    CHECK(v.x == -90.0f);
    CHECK(v.y == 0.0f);
}

TEST_CASE("movement: the same inputs give the same velocity") {
    Vec2 a;
    Vec2 b;
    for (int i = 0; i < 500; ++i) {
        const float ix = std::sin(static_cast<float>(i) * 0.37f);
        const float iy = i % 50 < 10 ? 0.0f : std::cos(static_cast<float>(i) * 0.11f) * 0.7f;
        a = step(a, ix * 0.7f, iy);
        b = step(b, ix * 0.7f, iy);
        REQUIRE(a.x == b.x);
        REQUIRE(a.y == b.y);
    }
}

TEST_CASE("movement: without a wall the walled step is the plain step, bit for bit") {
    const Vec2 starts[] = {{0.0f, 0.0f}, {40.0f, -20.0f}, {-90.0f, 0.0f}, {63.0f, 63.0f}};
    const float inputs[][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {-0.6f, 0.8f}, {0.3f, 0.3f}, {0.0f, -1.0f}, {0.70710678f, 0.70710678f}};
    for (const Vec2 start : starts) {
        for (const auto &input : inputs) {
            const Vec2 plain = step(start, input[0], input[1]);
            const Vec2 w = walled(start, input[0], input[1], IVec2{});
            CHECK(w.x == plain.x);
            CHECK(w.y == plain.y);
        }
    }
}

TEST_CASE("movement: a wall the player no longer pushes on changes nothing") {
    const Vec2 start{0.0f, 30.0f};
    const float inputs[][2] = {{-1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}, {-0.5f, 0.5f}};
    for (const auto &input : inputs) {
        const Vec2 plain = step(start, input[0], input[1]);
        const Vec2 w = walled(start, input[0], input[1], IVec2{1, 0});
        CHECK(w.x == plain.x);
        CHECK(w.y == plain.y);
    }
    const Vec2 plain = step(start, 1.0f, 0.0f);
    const Vec2 other = walled(start, 1.0f, 0.0f, IVec2{-1, 1});
    CHECK(other.x == plain.x);
    CHECK(other.y == plain.y);
}

TEST_CASE("movement: leaning on a wall, the free axis gets the whole acceleration") {
    const float d = std::sqrt(0.5f);
    const float wanted = d * 90.0f;
    const float reach = 900.0f * kTickSeconds;
    Vec2 v;
    int ticks = 0;
    while (v.y != wanted && ticks < 100) {
        v = walled(v, d, d, IVec2{1, 0});
        ++ticks;
        CHECK(v.y == doctest::Approx(std::min(reach * static_cast<float>(ticks), wanted)));
        CHECK(v.x == doctest::Approx(reach));
        CHECK(speed(v) <= 90.0f);
        v.x = 0.0f;                                              // the wall stops it again
    }
    CHECK(ticks == 5);
    Vec2 slow;
    int slow_ticks = 0;
    while (slow.y < wanted - 0.01f && slow_ticks < 100) {
        slow = step(slow, d, d);
        slow.x = 0.0f;
        ++slow_ticks;
    }
    CHECK(slow_ticks > 2 * ticks);
}

TEST_CASE("movement: leaning into a corner, each axis is tried from rest; pushing up works too") {
    const float d = std::sqrt(0.5f);
    const float reach = 900.0f * kTickSeconds;
    Vec2 v = walled(Vec2{50.0f, -50.0f}, -d, -d, IVec2{-1, -1});
    CHECK(v.x == doctest::Approx(-reach));
    CHECK(v.y == doctest::Approx(-reach));
    v = walled(Vec2{}, 0.1f, 0.05f, IVec2{0, 1});
    CHECK(v.x == 0.1f * 90.0f);
    CHECK(v.y == 0.05f * 90.0f);
    v = walled(Vec2{0.0f, 200.0f}, d, d, IVec2{1, 0});
    CHECK(v.y == doctest::Approx(200.0f - reach));
}

TEST_CASE("movement: input is clamped to the unit disc and non-finite input is zero") {
    using lam::detail::clamp_input;
    CHECK(clamp_input({0.3f, 0.4f}) == Vec2{0.3f, 0.4f});
    const Vec2 c = clamp_input({3.0f, 4.0f});
    CHECK(c.x == doctest::Approx(0.6f));
    CHECK(c.y == doctest::Approx(0.8f));
    CHECK(clamp_input({std::nanf(""), 1.0f}) == Vec2{});
    CHECK(clamp_input({1.0f, std::numeric_limits<float>::infinity()}) == Vec2{});
}
