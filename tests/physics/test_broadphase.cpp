// Broadphase and narrowphase, tested on the private types. Pairs are checked against a
// brute-force reference; shape tests against hand-worked cases.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/rng.h>
#include <lam/test/support.h>

#include "detail/physics_world.h"
#include "physics_app.h"

using namespace lam;
using lam::detail::Body;
using lam::detail::ContactInfo;
using lam::detail::PhysicsWorld;
using lam::detail::ShapeView;

namespace {

struct Scene {
    std::vector<Entity> entities;
    std::vector<bool> is_static;
};

// Random boxes and circles: small, big (many cells) and huge (oversize); a third static.
Scene random_scene(World &w, std::uint64_t seed, int n, float area, bool huge) {
    Scene s;
    Rng rng(seed);
    for (int i = 0; i < n; ++i) {
        const Entity e = w.create();
        w.add<Transform>(e, Transform{{rng.range(0.0f, area), rng.range(0.0f, area)}});
        Collider c;
        const float r = rng.unit();
        const float size = r < 0.7f ? rng.range(1.0f, 12.0f) : (r < 0.97f || !huge ? rng.range(20.0f, 90.0f) : rng.range(300.0f, 700.0f));
        if (i % 2 == 0) {
            c.shape = ShapeType::Box;
            c.half = {size, rng.range(1.0f, 12.0f)};
        } else {
            c.shape = ShapeType::Circle;
            c.radius = size;
        }
        c.layer = 1u << rng.below(3);
        c.mask = rng.chance(0.8f) ? 0xFFFFFFFFu : (1u << rng.below(3));
        w.add<Collider>(e, c);
        const bool st = i % 3 == 0;
        if (!st) w.add<Velocity>(e);
        s.entities.push_back(e);
        s.is_static.push_back(st);
    }
    return s;
}

std::vector<std::uint64_t> brute_pairs(World &w, const Scene &s) {
    std::vector<std::uint64_t> keys;
    for (std::size_t i = 0; i < s.entities.size(); ++i) {
        for (std::size_t j = i + 1; j < s.entities.size(); ++j) {
            if (s.is_static[i] && s.is_static[j]) continue;
            const Entity a = s.entities[i];
            const Entity b = s.entities[j];
            const Collider &ca = w.get<Collider>(a);
            const Collider &cb = w.get<Collider>(b);
            if ((ca.layer & cb.mask) == 0 || (cb.layer & ca.mask) == 0) continue;
            const Rect ra = collider_box(ca, w.get<Transform>(a).position).expanded(detail::kContactSkin);
            const Rect rb = collider_box(cb, w.get<Transform>(b).position).expanded(detail::kContactSkin);
            if (!ra.overlaps(rb)) continue;
            const std::uint32_t lo = std::min(a.index, b.index);
            const std::uint32_t hi = std::max(a.index, b.index);
            keys.push_back((static_cast<std::uint64_t>(lo) << 32) | hi);
        }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<std::uint64_t> grid_pairs(World &w, PhysicsWorld &pw) {
    detail::update_static(w, pw, false);
    detail::build_dynamic(w, pw);
    detail::find_pairs(pw);
    std::vector<std::uint64_t> keys;
    for (const detail::Pair &p : pw.pairs) {
        CHECK(detail::body_at(pw, p.a).entity.index < detail::body_at(pw, p.b).entity.index);
        keys.push_back(p.key);
    }
    return keys;
}

ShapeView box(Vec2 c, Vec2 half) {
    ShapeView v;
    v.type = ShapeType::Box;
    v.center = c;
    v.half = half;
    v.aabb = Rect::from_center(c, half);
    return v;
}

ShapeView circle(Vec2 c, float r) {
    ShapeView v;
    v.type = ShapeType::Circle;
    v.center = c;
    v.radius = r;
    v.half = {r, r};
    v.aabb = Rect::from_center(c, v.half);
    return v;
}

ShapeView polygon(Vec2 c, const Vec2 *local, int n) {
    ShapeView v;
    v.type = ShapeType::Polygon;
    v.center = c;
    v.local = local;
    v.count = static_cast<std::uint8_t>(n);
    Vec2 lo = local[0];
    Vec2 hi = local[0];
    for (int i = 1; i < n; ++i) {
        lo = min(lo, local[i]);
        hi = max(hi, local[i]);
    }
    v.aabb = Rect::from_min_max(c + lo, c + hi);
    v.half = v.aabb.size() * 0.5f;
    return v;
}

} // namespace

TEST_CASE("broadphase: every overlapping pair exactly once, sorted, at any cell size") {
    for (const float cell : {16.0f, 64.0f, 200.0f}) {
        for (const std::uint64_t seed : {1u, 2u, 3u}) {
            CAPTURE(cell);
            CAPTURE(seed);
            World w;
            PhysicsConfig cfg;
            cfg.cell_size = cell;
            PhysicsWorld pw(cfg);
            const Scene s = random_scene(w, seed, 300, 1500.0f, true);
            const std::vector<std::uint64_t> expected = brute_pairs(w, s);
            const std::vector<std::uint64_t> got = grid_pairs(w, pw);
            CHECK(got == expected);
            CHECK(std::is_sorted(got.begin(), got.end()));
            CHECK(std::adjacent_find(got.begin(), got.end()) == got.end());
            CHECK(pw.dropped_pairs == 0);
            CHECK(expected.size() > 50);
        }
    }
}

TEST_CASE("broadphase: past pair_capacity the smallest keys are kept and the rest counted") {
    World w;
    const Scene s = random_scene(w, 7, 300, 800.0f, false);
    const std::vector<std::uint64_t> all = brute_pairs(w, s);
    REQUIRE(all.size() > 40);
    PhysicsConfig cfg;
    cfg.pair_capacity = 25;
    PhysicsWorld pw(cfg);
    const std::vector<std::uint64_t> kept = grid_pairs(w, pw);
    REQUIRE(kept.size() == 25);
    CHECK(std::equal(kept.begin(), kept.end(), all.begin()));
    CHECK(pw.dropped_pairs == all.size() - 25);
}

TEST_CASE("broadphase: two big bodies sharing many cells make one pair") {
    World w;
    PhysicsConfig cfg;
    cfg.cell_size = 16.0f;
    PhysicsWorld pw(cfg);
    for (int i = 0; i < 2; ++i) {
        const Entity e = w.create();
        w.add<Transform>(e, Transform{{static_cast<float>(i) * 30.0f, 0}});
        Collider c;
        c.half = {50, 50};                                     // 7 x 7 cells each, 4 x 7 shared
        w.add<Collider>(e, c);
        w.add<Velocity>(e);
    }
    const std::vector<std::uint64_t> got = grid_pairs(w, pw);
    CHECK(got.size() == 1);
}

TEST_CASE("broadphase: the static grid is rebuilt only when static colliders change") {
    lam::test::PhysicsApp p;
    World &w = p.world();
    std::vector<Entity> walls;
    for (int i = 0; i < 20; ++i) walls.push_back(lam::test::spawn_static(w, {static_cast<float>(i) * 20.0f, 0}, lam::test::box_collider({8, 8})));
    const Entity mover = lam::test::spawn_kinematic(w, {0, 50}, lam::test::box_collider({4, 4}), {30, 0});
    p.ticks(2);
    PhysicsWorld &pw = w.resources().get<PhysicsWorld>();
    const std::uint64_t builds = pw.static_builds;
    p.ticks(30);                                                 // only the kinematic body moves
    CHECK(pw.static_builds == builds);
    CHECK(pw.dyn_builds >= 30);
    // Dynamic bodies coming and going do not touch it either.
    const Entity extra = lam::test::spawn_kinematic(w, {5, 80}, lam::test::box_collider({4, 4}));
    p.ticks(3);
    w.destroy(extra);
    p.ticks(3);
    CHECK(pw.static_builds == builds);
    // A static that moves, changes shape, changes layer, appears or goes: one rebuild each.
    w.get<Transform>(walls[3]).position.x += 1.0f;
    p.ticks(3);
    CHECK(pw.static_builds == builds + 1);
    w.get<Collider>(walls[4]).half = {6, 6};
    p.ticks(3);
    CHECK(pw.static_builds == builds + 2);
    w.get<Collider>(walls[5]).layer = 2;
    p.ticks(3);
    CHECK(pw.static_builds == builds + 3);
    lam::test::spawn_static(w, {500, 0}, lam::test::box_collider({8, 8}));
    p.ticks(3);
    CHECK(pw.static_builds == builds + 4);
    w.destroy(walls[6]);
    p.ticks(3);
    CHECK(pw.static_builds == builds + 5);
    // A static that becomes kinematic (gains Velocity) leaves the static grid.
    w.add<Velocity>(walls[7]);
    p.ticks(3);
    CHECK(pw.static_builds == builds + 6);
    CHECK(pw.statics.size() == 19);
    (void)mover;
}

TEST_CASE("shapes: boxes, circles and polygons collide with a normal from a to b") {
    ContactInfo ci;
    // Box-box: least penetration axis.
    REQUIRE(detail::collide(box({0, 0}, {5, 5}), box({8, 1}, {5, 5}), ci));
    CHECK(ci.normal == Vec2{1, 0});
    CHECK(ci.depth == doctest::Approx(2.0f));
    CHECK_FALSE(detail::collide(box({0, 0}, {5, 5}), box({10, 0}, {5, 5}), ci));   // touching
    CHECK(detail::collide(box({0, 0}, {5, 5}), box({10, 0}, {5, 5}), ci, 0.1f));   // within skin
    CHECK(ci.depth == doctest::Approx(0.0f));
    // Circle-circle.
    REQUIRE(detail::collide(circle({0, 0}, 3), circle({0, 5}, 3), ci));
    CHECK(ci.normal == Vec2{0, 1});
    CHECK(ci.depth == doctest::Approx(1.0f));
    CHECK_FALSE(detail::collide(circle({0, 0}, 3), circle({0, 6}, 3), ci));
    // Box-circle both ways: the normal flips.
    REQUIRE(detail::collide(box({0, 0}, {5, 5}), circle({7, 0}, 3), ci));
    CHECK(ci.normal.x == doctest::Approx(1.0f));
    CHECK(ci.depth == doctest::Approx(1.0f));
    REQUIRE(detail::collide(circle({7, 0}, 3), box({0, 0}, {5, 5}), ci));
    CHECK(ci.normal.x == doctest::Approx(-1.0f));
    // Circle near a box corner: the normal points along the diagonal.
    REQUIRE(detail::collide(box({0, 0}, {5, 5}), circle({7, 7}, 3), ci));
    CHECK(ci.normal.x == doctest::Approx(0.70710678f));
    CHECK(ci.normal.y == doctest::Approx(0.70710678f));
    CHECK_FALSE(detail::collide(box({0, 0}, {5, 5}), circle({8, 8}, 3), ci));
    // Polygon (a triangle pointing up) against a box resting on its apex.
    const Vec2 tri[3] = {{-10, 5}, {10, 5}, {0, -5}};
    REQUIRE(detail::collide(polygon({0, 0}, tri, 3), box({0, -6}, {2, 2}), ci));
    CHECK(ci.normal.y < 0.0f);
    CHECK_FALSE(detail::collide(polygon({0, 0}, tri, 3), box({9, -4}, {2, 2}), ci));   // beside the slope
    // Circle against polygon.
    REQUIRE(detail::collide(circle({0, -7}, 3), polygon({0, 0}, tri, 3), ci));
    CHECK(ci.normal.y > 0.0f);                                   // from the circle down into the triangle
}

TEST_CASE("shapes: sweeps report the first contact, the normal and starting inside") {
    // A ray into a box face.
    std::optional<SegmentHit> h = detail::sweep_shape(box({10, 0}, {2, 2}), {0, 0}, {20, 0}, 0.0f);
    REQUIRE(h.has_value());
    CHECK(h->t == doctest::Approx(0.4f));
    CHECK(h->normal == Vec2{-1, 0});
    // A circle round the corner: rounded Minkowski sum.
    h = detail::sweep_shape(box({10, 0}, {2, 2}), {0, 4}, {20, 4}, 1.5f);
    CHECK_FALSE(h.has_value());                                  // 2 px above the box top + 1.5 radius
    h = detail::sweep_shape(box({10, 0}, {2, 2}), {0, 3}, {20, 3}, 1.5f);
    REQUIRE(h.has_value());
    CHECK(h->normal.x < 0.0f);
    CHECK(h->normal.y > 0.0f);
    // Into a circle.
    h = detail::sweep_shape(circle({10, 0}, 2), {0, 0}, {20, 0}, 1.0f);
    REQUIRE(h.has_value());
    CHECK(h->point.x == doctest::Approx(7.0f));
    // Into a polygon edge.
    const Vec2 tri[3] = {{-10, 5}, {10, 5}, {0, -5}};
    h = detail::sweep_shape(polygon({0, 0}, tri, 3), {0, -20}, {0, 20}, 0.0f);
    REQUIRE(h.has_value());
    CHECK(h->point.y == doctest::Approx(-5.0f).epsilon(1e-4));
    // Starting inside: t = 0, zero normal.
    h = detail::sweep_shape(box({0, 0}, {5, 5}), {1, 1}, {20, 1}, 0.0f);
    REQUIRE(h.has_value());
    CHECK(h->t == 0.0f);
    CHECK(h->normal == Vec2{});
    // Non-finite: nothing.
    CHECK_FALSE(detail::sweep_shape(box({0, 0}, {5, 5}), {std::nanf(""), 0}, {20, 1}, 0.0f).has_value());
}

TEST_CASE("shapes: make_shape places colliders; bad polygons fall back to the box") {
    PolygonShapes polys;
    const Vec2 quad[4] = {{-2, -2}, {2, -2}, {2, 2}, {-2, 2}};
    const std::uint16_t id = polys.add(quad);
    Collider c;
    c.shape = ShapeType::Polygon;
    c.polygon = id;
    c.offset = {1, 0};
    ShapeView v = detail::make_shape(c, {10, 10}, &polys);
    CHECK(v.type == ShapeType::Polygon);
    CHECK(v.aabb == Rect{9, 8, 4, 4});
    c.polygon = 99;                                              // unknown id: the Box of `half`
    v = detail::make_shape(c, {10, 10}, &polys);
    CHECK(v.type == ShapeType::Box);
    CHECK(v.aabb == Rect::from_center({11, 10}, c.half));
    Collider round;
    round.shape = ShapeType::Circle;
    round.radius = 3;
    v = detail::make_shape(round, {0, 0}, nullptr);
    CHECK(v.aabb == Rect{-3, -3, 6, 6});
    CHECK(collider_box(round, {0, 0}) == v.aabb);
}
