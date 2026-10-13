// Mat2x3 and Transform2D.
#include <doctest/doctest.h>

#include <limits>

#include <lam/math/rng.h>
#include <lam/math/transform.h>

using namespace lam;

namespace {
void check_near(Vec2 a, Vec2 b, double eps = 1e-4) {
    CHECK(a.x == doctest::Approx(b.x).epsilon(eps).scale(1.0));
    CHECK(a.y == doctest::Approx(b.y).epsilon(eps).scale(1.0));
}
bool is_identity(const Mat2x3 &m) {
    return m.a == 1 && m.b == 0 && m.c == 0 && m.d == 1 && m.tx == 0 && m.ty == 0;
}
} // namespace

static_assert(Mat2x3::identity().apply({3, 4}) == Vec2{3, 4});

TEST_CASE("transform: trs scales, then rotates, then translates") {
    const Mat2x3 m = Mat2x3::trs({10, 20}, kPi * 0.5f, {2, 3});
    check_near(m.apply({1, 0}), {10, 22});
    check_near(m.apply({0, 1}), {7, 20});
    check_near(m.apply({0, 0}), {10, 20});
    check_near(m.apply_dir({1, 0}), {0, 2}); // no translation
    const Transform2D t{{10, 20}, kPi * 0.5f, {2, 3}};
    check_near(t.matrix().apply({1, 1}), m.apply({1, 1}));
}

TEST_CASE("transform: inverse undoes the matrix; singular gives identity") {
    Rng rng(9);
    for (int i = 0; i < 200; ++i) {
        const Mat2x3 m = Mat2x3::trs({rng.range(-100, 100), rng.range(-100, 100)}, rng.range(-4, 4),
                                     {rng.range(0.5f, 3), rng.range(0.5f, 3)});
        const Vec2 p{rng.range(-50, 50), rng.range(-50, 50)};
        const Vec2 back = m.inverse().apply(m.apply(p));
        REQUIRE(back.x == doctest::Approx(p.x).epsilon(1e-3).scale(1.0));
        REQUIRE(back.y == doctest::Approx(p.y).epsilon(1e-3).scale(1.0));
    }
    CHECK(is_identity(Mat2x3::trs({5, 5}, 1.0f, {0, 1}).inverse()));
    Mat2x3 bad;
    bad.a = std::numeric_limits<float>::quiet_NaN();
    CHECK(is_identity(bad.inverse()));
}

TEST_CASE("transform: operator* composes right to left") {
    const Mat2x3 a = Mat2x3::trs({3, -2}, 0.7f, {2, 1});
    const Mat2x3 b = Mat2x3::trs({-1, 4}, -1.3f, {0.5f, 3});
    for (Vec2 p : {Vec2{0, 0}, Vec2{1, 2}, Vec2{-7, 3}})
        check_near((a * b).apply(p), a.apply(b.apply(p)));
    check_near((Mat2x3::identity() * a).apply({2, 2}), a.apply({2, 2}));
}

TEST_CASE("transform: combine applies the parent to the child") {
    const Transform2D parent{{10, 0}, kPi * 0.5f, {2, 2}};
    const Transform2D local{{1, 0}, 0.5f, {3, 1}};
    const Transform2D world = combine(parent, local);
    check_near(world.position, {10, 2});
    CHECK(world.rotation == doctest::Approx(kPi * 0.5f + 0.5f));
    CHECK(world.scale == Vec2{6, 2});
    const Transform2D same = combine(Transform2D{}, local);
    check_near(same.position, local.position);
    CHECK(same.rotation == doctest::Approx(local.rotation));
    CHECK(same.scale == local.scale);
}

TEST_CASE("transform: lerp takes the shortest arc") {
    const Transform2D a{{0, 0}, 3.0f, {1, 1}};
    const Transform2D b{{10, 20}, -3.0f, {3, 5}};
    const Transform2D mid = lerp(a, b, 0.5f);
    check_near(mid.position, {5, 10});
    check_near(mid.scale, {2, 3});
    // From 3.0 to -3.0 the short way crosses pi (0.283 rad), not zero (6 rad).
    CHECK(mid.rotation == doctest::Approx(3.0f + (2.0f * kPi - 6.0f) * 0.5f));
    CHECK(lerp(a, b, 0.0f).rotation == doctest::Approx(3.0f));
    CHECK(wrap_angle(lerp(a, b, 1.0f).rotation - b.rotation) ==
          doctest::Approx(0.0f).epsilon(1e-5));
    const Transform2D c{{0, 0}, 0.25f, {1, 1}};
    const Transform2D d{{0, 0}, -0.25f, {1, 1}};
    CHECK(lerp(c, d, 0.5f).rotation == doctest::Approx(0.0f).epsilon(1e-6));
}
