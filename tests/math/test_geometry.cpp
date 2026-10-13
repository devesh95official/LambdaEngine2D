// Overlap, cone, segment and swept-circle tests.
#include <doctest/doctest.h>

#include <cmath>

#include <lam/math/geometry.h>

using namespace lam;

namespace {
void check_near(Vec2 a, Vec2 b, double eps = 1e-4) {
    CHECK(a.x == doctest::Approx(b.x).epsilon(eps).scale(1.0));
    CHECK(a.y == doctest::Approx(b.y).epsilon(eps).scale(1.0));
}
} // namespace

TEST_CASE("geometry: circle overlaps; touching is not overlap") {
    CHECK(overlaps(Circle{{0, 0}, 1}, Circle{{1.5f, 0}, 1}));
    CHECK_FALSE(overlaps(Circle{{0, 0}, 1}, Circle{{2, 0}, 1}));
    CHECK_FALSE(overlaps(Circle{{0, 0}, 1}, Circle{{5, 0}, 1}));
    const Rect r{0, 0, 10, 10};
    CHECK(overlaps(Circle{{5, 5}, 1}, r));         // centre inside
    CHECK(overlaps(Circle{{-1, 5}, 2}, r));        // crosses the left edge
    CHECK_FALSE(overlaps(Circle{{-2, 5}, 2}, r));  // touches the left edge
    CHECK_FALSE(overlaps(Circle{{-2, -2}, 2}, r)); // near the corner, outside
    CHECK(overlaps(Circle{{-1, -1}, 2}, r));
}

TEST_CASE("geometry: contains counts the edge as inside") {
    CHECK(contains(Circle{{0, 0}, 1}, {1, 0}));
    CHECK(contains(Circle{{0, 0}, 1}, {0, 0}));
    CHECK_FALSE(contains(Circle{{0, 0}, 1}, {1.01f, 0}));
}

TEST_CASE("geometry: point_in_cone") {
    const Vec2 o{0, 0};
    const Vec2 dir{1, 0};
    CHECK(point_in_cone(o, dir, 10, kPi / 4, {5, 0}));
    CHECK(point_in_cone(o, dir, 10, kPi / 4, {5, 4}));        // 38.7 degrees
    CHECK_FALSE(point_in_cone(o, dir, 10, kPi / 4, {5, 6}));  // 50.2 degrees
    CHECK_FALSE(point_in_cone(o, dir, 10, kPi / 4, {11, 0})); // out of range
    CHECK_FALSE(point_in_cone(o, dir, 10, kPi / 4, {-5, 0}));
    CHECK(point_in_cone(o, dir, 10, kPi / 4, o)); // the origin itself
    CHECK(point_in_cone({3, 3}, {0, 1}, 5, 0.1f, {3, 7}));
    CHECK(point_in_cone(o, dir, 10, kPi, {-5, 0.1f})); // a full circle
}

TEST_CASE("geometry: a segment hits a rect with the normal of the face it enters") {
    const Rect r{0, 0, 10, 10};
    struct Case {
        Segment s;
        float t;
        Vec2 point, normal;
    };
    const Case cases[] = {
        {{{-5, 5}, {5, 5}}, 0.5f, {0, 5}, {-1, 0}},  // from the left
        {{{15, 5}, {5, 5}}, 0.5f, {10, 5}, {1, 0}},  // from the right
        {{{5, -5}, {5, 5}}, 0.5f, {5, 0}, {0, -1}},  // from above (+y down)
        {{{5, 15}, {5, 5}}, 0.5f, {5, 10}, {0, 1}},  // from below
        {{{-5, -2}, {5, 8}}, 0.5f, {0, 3}, {-1, 0}}, // diagonal, enters the left face
        {{{2, -4}, {6, 4}}, 0.5f, {4, 0}, {0, -1}},  // diagonal, enters the top face
    };
    for (const Case &c : cases) {
        CAPTURE(c.point.x);
        CAPTURE(c.point.y);
        const auto hit = intersect(c.s, r);
        REQUIRE(hit.has_value());
        CHECK(hit->t == doctest::Approx(c.t));
        check_near(hit->point, c.point);
        CHECK(hit->normal == c.normal);
    }
    CHECK_FALSE(intersect(Segment{{-5, -5}, {-5, 15}}, r).has_value()); // passes beside
    CHECK_FALSE(intersect(Segment{{-5, 5}, {-1, 5}}, r).has_value());   // stops short
    CHECK_FALSE(intersect(Segment{{20, 5}, {30, 5}}, r).has_value());   // points away
    const auto inside = intersect(Segment{{5, 5}, {20, 5}}, r);
    REQUIRE(inside.has_value());
    CHECK(inside->t == 0.0f);
    CHECK(inside->point == Vec2{5, 5});
    CHECK(inside->normal == Vec2{});
}

TEST_CASE("geometry: a segment hits a circle") {
    const Circle c{{10, 0}, 2};
    const auto hit = intersect(Segment{{0, 0}, {20, 0}}, c);
    REQUIRE(hit.has_value());
    CHECK(hit->t == doctest::Approx(0.4f));
    check_near(hit->point, {8, 0});
    check_near(hit->normal, {-1, 0});
    const auto above = intersect(Segment{{10, -10}, {10, 10}}, c);
    REQUIRE(above.has_value());
    check_near(above->point, {10, -2});
    check_near(above->normal, {0, -1});
    CHECK_FALSE(intersect(Segment{{0, 5}, {20, 5}}, c).has_value());  // misses
    CHECK_FALSE(intersect(Segment{{0, 0}, {5, 0}}, c).has_value());   // too short
    CHECK_FALSE(intersect(Segment{{0, 0}, {-20, 0}}, c).has_value()); // moving away
    CHECK_FALSE(intersect(Segment{{0, 0}, {0, 0}}, c).has_value());   // a point outside
    const auto inside = intersect(Segment{{10, 1}, {30, 1}}, c);
    REQUIRE(inside.has_value());
    CHECK(inside->t == 0.0f);
    CHECK(inside->normal == Vec2{});
}

TEST_CASE("geometry: sweep_circle_rect uses flat faces and rounded corners") {
    const Rect r{0, 0, 10, 10};
    const float radius = 2;
    // A face: the centre stops `radius` before the left edge.
    const auto face = sweep_circle_rect({-10, 5}, {10, 5}, radius, r);
    REQUIRE(face.has_value());
    CHECK(face->t == doctest::Approx(0.4f));
    check_near(face->point, {-2, 5});
    CHECK(face->normal == Vec2{-1, 0});
    // A corner: straight at (0, 0) along the diagonal, contact at distance `radius` from it.
    const auto corner = sweep_circle_rect({-10, -10}, {0, 0}, radius, r);
    REQUIRE(corner.has_value());
    const float s = std::sqrt(2.0f);
    CHECK(corner->t == doctest::Approx((10.0f - s) / 10.0f));
    check_near(corner->point, {-s, -s});
    check_near(corner->normal, {-1 / s, -1 / s});
    // Every corner of the box, approached diagonally.
    const Vec2 corners[] = {{0, 0}, {10, 0}, {0, 10}, {10, 10}};
    for (Vec2 k : corners) {
        const Vec2 away = normalize_or(k - r.center());
        const auto h = sweep_circle_rect(k + away * 20, r.center(), radius, r);
        REQUIRE(h.has_value());
        CHECK(distance(h->point, k) == doctest::Approx(radius));
        check_near(h->normal, away);
    }
    // Through the expanded box's corner square but outside the rounded corner: a miss.
    CHECK_FALSE(sweep_circle_rect({-2.5f, -0.5f}, {-0.5f, -2.5f}, radius, r).has_value());
    // Starting in contact reports t = 0.
    const auto start = sweep_circle_rect({-1, 5}, {-10, 5}, radius, r);
    REQUIRE(start.has_value());
    CHECK(start->t == 0.0f);
    CHECK(start->normal == Vec2{});
    CHECK_FALSE(sweep_circle_rect({-10, 5}, {-5, 5}, radius, r).has_value());
}
