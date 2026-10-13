// Vec2 and IVec2.
#include <doctest/doctest.h>

#include <limits>

#include <lam/math/vec2.h>

using namespace lam;

static_assert(Vec2{1, 2} + Vec2{3, 4} == Vec2{4, 6});
static_assert(Vec2{1, 2} * 2.0f == Vec2{2, 4});
static_assert(Vec2{2, 3} * Vec2{4, 5} == Vec2{8, 15});
static_assert(dot(Vec2{1, 2}, Vec2{3, 4}) == 11.0f);
static_assert(cross(Vec2{1, 0}, Vec2{0, 1}) == 1.0f); // down is clockwise from right on screen
static_assert(perp(Vec2{1, 0}) == Vec2{0, 1});
static_assert(manhattan(IVec2{0, 0}, IVec2{3, -4}) == 7);
static_assert(chebyshev(IVec2{0, 0}, IVec2{3, -4}) == 4);
static_assert(to_vec2(IVec2{2, -3}) == Vec2{2, -3});

TEST_CASE("vec2: arithmetic operators") {
    Vec2 v{1, 2};
    v += {2, 2};
    CHECK(v == Vec2{3, 4});
    v -= {1, 1};
    CHECK(v == Vec2{2, 3});
    v *= 2.0f;
    CHECK(v == Vec2{4, 6});
    CHECK(-v == Vec2{-4, -6});
    CHECK(v / 2.0f == Vec2{2, 3});
    CHECK(0.5f * v == Vec2{2, 3});
    CHECK(v - Vec2{4, 6} == Vec2{});
    CHECK(IVec2{1, 2} + IVec2{3, 4} == IVec2{4, 6});
    CHECK(IVec2{1, 2} - IVec2{3, 4} == IVec2{-2, -2});
    CHECK(-IVec2{1, 2} == IVec2{-1, -2});
    CHECK(IVec2{1, 2} * 3 == IVec2{3, 6});
}

TEST_CASE("vec2: lengths and distances") {
    CHECK(length(Vec2{3, 4}) == doctest::Approx(5.0f));
    CHECK(length_sq(Vec2{3, 4}) == 25.0f);
    CHECK(distance(Vec2{1, 1}, Vec2{4, 5}) == doctest::Approx(5.0f));
    CHECK(distance_sq(Vec2{1, 1}, Vec2{4, 5}) == 25.0f);
}

TEST_CASE("vec2: normalize_or falls back for tiny and non-finite vectors") {
    const Vec2 n = normalize_or({3, 4});
    CHECK(n.x == doctest::Approx(0.6f));
    CHECK(n.y == doctest::Approx(0.8f));
    CHECK(normalize_or({0, 0}) == Vec2{});
    CHECK(normalize_or({1e-4f, 0}, {0, 1}) == Vec2{0, 1});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(normalize_or({nan, 1}, {1, 0}) == Vec2{1, 0});
    CHECK(normalize_or({inf, 1}, {1, 0}) == Vec2{1, 0});
}

TEST_CASE("vec2: clamp_length keeps the direction") {
    const Vec2 c = clamp_length({30, 40}, 5.0f);
    CHECK(c.x == doctest::Approx(3.0f));
    CHECK(c.y == doctest::Approx(4.0f));
    CHECK(clamp_length({1, 1}, 5.0f) == Vec2{1, 1});
    CHECK(clamp_length({1, 1}, 0.0f) == Vec2{});
    CHECK(clamp_length({1, 1}, -2.0f) == Vec2{});
}

TEST_CASE("vec2: rotate, angle_of and from_angle agree") {
    const Vec2 r = rotate({1, 0}, kPi * 0.5f);
    CHECK(r.x == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(r.y == doctest::Approx(1.0f));
    CHECK(angle_of({0, 1}) == doctest::Approx(kPi * 0.5f));
    CHECK(angle_of({-1, 0}) == doctest::Approx(kPi));
    const Vec2 f = from_angle(0.0f, 2.0f);
    CHECK(f.x == doctest::Approx(2.0f));
    CHECK(f.y == doctest::Approx(0.0f));
    for (int i = -30; i <= 30; ++i) {
        const float a = static_cast<float>(i) * 0.1f;
        const Vec2 v = from_angle(a, 3.0f);
        REQUIRE(length(v) == doctest::Approx(3.0f));
        REQUIRE(wrap_angle(angle_of(v) - a) == doctest::Approx(0.0f).epsilon(1e-5));
        const Vec2 back = rotate(rotate(v, a), -a);
        REQUIRE(back.x == doctest::Approx(v.x).epsilon(1e-5));
        REQUIRE(back.y == doctest::Approx(v.y).epsilon(1e-5));
    }
}

TEST_CASE("vec2: min, max, floor, round and floor_to_ivec") {
    CHECK(min(Vec2{1, 5}, Vec2{3, 2}) == Vec2{1, 2});
    CHECK(max(Vec2{1, 5}, Vec2{3, 2}) == Vec2{3, 5});
    CHECK(floor(Vec2{-0.5f, 1.5f}) == Vec2{-1, 1});
    CHECK(round(Vec2{-0.5f, 1.49f}) == Vec2{0, 1}); // floor(v + 0.5)
    CHECK(round(Vec2{2.5f, -2.6f}) == Vec2{3, -3});
    CHECK(floor_to_ivec(Vec2{-0.1f, 2.9f}) == IVec2{-1, 2});
    CHECK(is_finite(Vec2{1, 2}));
    CHECK_FALSE(is_finite(Vec2{std::numeric_limits<float>::infinity(), 0}));
    CHECK(lerp(Vec2{0, 0}, Vec2{10, 20}, 0.5f) == Vec2{5, 10});
}

TEST_CASE("vec2: approach moves along the line and never overshoots") {
    const Vec2 a = approach(Vec2{0, 0}, Vec2{3, 4}, 1.0f);
    CHECK(a.x == doctest::Approx(0.6f));
    CHECK(a.y == doctest::Approx(0.8f));
    CHECK(approach(Vec2{0, 0}, Vec2{3, 4}, 5.0f) == Vec2{3, 4});
    CHECK(approach(Vec2{0, 0}, Vec2{3, 4}, 100.0f) == Vec2{3, 4});
    CHECK(approach(Vec2{1, 1}, Vec2{3, 4}, 0.0f) == Vec2{1, 1});
    CHECK(approach(Vec2{1, 1}, Vec2{3, 4}, -1.0f) == Vec2{1, 1});
    Vec2 p{0, 0};
    for (int i = 0; i < 20; ++i)
        p = approach(p, Vec2{3, 4}, 0.7f);
    CHECK(p == Vec2{3, 4});
}
