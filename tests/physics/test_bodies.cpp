// Rigid bodies, the solver and contact events, run through PhysicsPlugin.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <lam/core/debug.h>
#include <lam/ecs/snapshot.h>
#include <lam/math/rng.h>
#include <lam/test/support.h>

#include "physics_app.h"
#include "physics_test_util.h"

using namespace lam;
using lam::test::box_collider;
using lam::test::circle_collider;
using lam::test::PhysicsApp;
using lam::test::pos_of;
using lam::test::spawn_dynamic;
using lam::test::spawn_kinematic;
using lam::test::spawn_static;

namespace {

PhysicsConfig with_gravity(float g) {
    PhysicsConfig c;
    c.gravity = {0.0f, g};
    return c;
}

// 20 x 12 tiles of 16 px, solid floor on row 10 (y = 160) and walls in columns 0 and 19.
void floor_room(TileWorld &tw) {
    tw.resize(20, 12, 16, {});
    for (int x = 0; x < 20; ++x) {
        tw.set({x, 10}, TileShape::Solid, 0);
        tw.set({x, 11}, TileShape::Solid, 0);
    }
    for (int y = 0; y < 12; ++y) {
        tw.set({0, y}, TileShape::Solid, 0);
        tw.set({19, y}, TileShape::Solid, 0);
    }
}

} // namespace

TEST_CASE("bodies: Velocity alone moves an entity kinematically, with or without a collider") {
    PhysicsApp p;
    World &w = p.world();
    const Entity bare = w.create();
    w.add<Transform>(bare, Transform{{10, 10}});
    w.add<Velocity>(bare, Velocity{{60, -30}});
    const Entity body = spawn_kinematic(w, {0, 0}, box_collider({4, 4}), {120, 0});
    p.ticks(60);
    CHECK(pos_of(w, bare).x == doctest::Approx(70.0f));
    CHECK(pos_of(w, bare).y == doctest::Approx(-20.0f));
    CHECK(pos_of(w, body).x == doctest::Approx(120.0f));
    // A Static RigidBody never moves, whatever its Velocity says.
    const Entity pinned = spawn_static(w, {5, 5}, box_collider({4, 4}));
    w.add<RigidBody>(pinned, RigidBody{.type = BodyType::Static});
    w.get<Velocity>(pinned).linear = {100, 100};
    p.ticks(10);
    CHECK(pos_of(w, pinned) == Vec2{5, 5});
}

TEST_CASE("bodies: a dynamic box falls, lands flush on the floor and stays at rest") {
    PhysicsApp p(with_gravity(600.0f));
    floor_room(p.tiles());
    World &w = p.world();
    const Entity box = spawn_dynamic(w, {40, 20}, box_collider({4, 4}));
    p.ticks(10);
    CHECK(w.get<Velocity>(box).linear.y > 0.0f);                // falling
    p.ticks(110);
    CHECK(pos_of(w, box).y + 4.0f <= 160.0f);
    CHECK(pos_of(w, box).y + 4.0f > 159.99f);
    CHECK(w.get<Velocity>(box).linear.y == 0.0f);
    const Vec2 rest = pos_of(w, box);
    p.ticks(60);
    CHECK(pos_of(w, box) == rest);
}

TEST_CASE("bodies: no tunnelling through a one-tile wall at any speed") {
    for (const float speed : {100.0f, 1000.0f, 10000.0f, 200000.0f}) {
        CAPTURE(speed);
        PhysicsApp p;
        TileWorld &tw = p.tiles();
        tw.resize(40, 10, 16, {});
        for (int y = 0; y < 10; ++y) tw.set({20, y}, TileShape::Solid, 0);   // x in [320, 336)
        World &w = p.world();
        const Entity right = spawn_dynamic(w, {100, 80}, box_collider({3, 3}), {}, {speed, 0});
        const Entity circle = spawn_dynamic(w, {100, 40}, circle_collider(2.5f), {}, {speed, speed * 0.01f});
        const Entity left = spawn_dynamic(w, {550, 80}, box_collider({3, 3}), {}, {-speed, 0});
        p.ticks(30);
        CHECK(pos_of(w, right).x + 3.0f <= 320.0f);
        CHECK(pos_of(w, circle).x + 2.5f <= 320.0f);
        CHECK(pos_of(w, left).x - 3.0f >= 336.0f);
    }
}

TEST_CASE("bodies: restitution bounces a dropped ball lower each time; without it the ball stops") {
    PhysicsApp p(with_gravity(600.0f));
    floor_room(p.tiles());
    World &w = p.world();
    const Entity bouncy = spawn_dynamic(w, {40, 40}, circle_collider(4), RigidBody{.restitution = 0.6f});
    const Entity dead = spawn_dynamic(w, {80, 40}, circle_collider(4), RigidBody{.restitution = 0.0f});
    float lowest_vy = 0.0f;
    int bounces = 0;
    float prev_vy = 0.0f;
    for (int i = 0; i < 240; ++i) {
        p.ticks(1);
        const float vy = w.get<Velocity>(bouncy).linear.y;
        if (prev_vy > 0.0f && vy < 0.0f) ++bounces;
        if (vy < lowest_vy) lowest_vy = vy;
        prev_vy = vy;
        CHECK(w.get<Velocity>(dead).linear.y >= 0.0f);          // never goes up
    }
    CHECK(bounces >= 2);
    CHECK(lowest_vy < -50.0f);
    CHECK(pos_of(w, dead).y == doctest::Approx(156.0f).epsilon(1e-4));
}

TEST_CASE("bodies: friction slows a box sliding on the floor; a frictionless one keeps its speed") {
    PhysicsApp p(with_gravity(600.0f));
    floor_room(p.tiles());
    World &w = p.world();
    const Entity rough = spawn_dynamic(w, {40, 155}, box_collider({4, 4}), RigidBody{.friction = 0.5f}, {120, 0});
    const Entity slick = spawn_dynamic(w, {40, 135}, box_collider({4, 4}), RigidBody{.friction = 0.0f}, {50, 0});
    w.get<Transform>(slick).position = {40, 100};
    p.ticks(30);
    // Coulomb: |dv| per tick = friction * g * dt = 0.5 * 600 / 60 = 5 px/s, from 120: 24 ticks.
    CHECK(w.get<Velocity>(rough).linear.x == 0.0f);
    CHECK(w.get<Velocity>(slick).linear.x == doctest::Approx(50.0f));
}

TEST_CASE("bodies: a head-on collision conserves momentum; restitution 1 swaps equal masses") {
    PhysicsApp p;
    World &w = p.world();
    const Entity a = spawn_dynamic(w, {0, 0}, box_collider({4, 4}), RigidBody{.mass = 1, .restitution = 1}, {60, 0});
    const Entity b = spawn_dynamic(w, {30, 0}, box_collider({4, 4}), RigidBody{.mass = 1, .restitution = 1}, {-60, 0});
    p.ticks(30);
    CHECK(w.get<Velocity>(a).linear.x == doctest::Approx(-60.0f).epsilon(0.02));
    CHECK(w.get<Velocity>(b).linear.x == doctest::Approx(60.0f).epsilon(0.02));
    CHECK(pos_of(w, a).x < pos_of(w, b).x);

    const Entity heavy = spawn_dynamic(w, {0, 100}, box_collider({4, 4}), RigidBody{.mass = 3}, {40, 0});
    const Entity light = spawn_dynamic(w, {40, 100}, box_collider({4, 4}), RigidBody{.mass = 1}, {0, 0});
    p.ticks(60);
    const float va = w.get<Velocity>(heavy).linear.x;
    const float vb = w.get<Velocity>(light).linear.x;
    CHECK(3.0f * va + vb == doctest::Approx(120.0f).epsilon(0.01));   // momentum
    CHECK(va == doctest::Approx(vb).epsilon(0.05));                    // inelastic: moving together
}

TEST_CASE("bodies: a stack of boxes on the floor settles without sinking or exploding") {
    PhysicsApp p(with_gravity(600.0f));
    floor_room(p.tiles());
    World &w = p.world();
    std::vector<Entity> stack;
    for (int i = 0; i < 5; ++i) {
        stack.push_back(spawn_dynamic(w, {80, 152.0f - 16.5f * static_cast<float>(i)}, box_collider({8, 8})));
    }
    p.ticks(300);
    for (int i = 0; i < 5; ++i) {
        CAPTURE(i);
        const float expected = 152.0f - 16.0f * static_cast<float>(i);
        CHECK(pos_of(w, stack[static_cast<std::size_t>(i)]).y == doctest::Approx(expected).epsilon(0.004));
        CHECK(pos_of(w, stack[static_cast<std::size_t>(i)]).x == doctest::Approx(80.0f).epsilon(1e-3));
        CHECK(std::fabs(w.get<Velocity>(stack[static_cast<std::size_t>(i)]).linear.y) < 15.0f);
    }
    // Settled, the contacts hold: no Started/Ended flicker while the stack rests.
    p.events().clear();
    p.ticks(120);
    CHECK(p.events().started.empty());
    CHECK(p.events().ended.empty());
}

TEST_CASE("bodies: a messy pile of boxes and balls comes to rest and stops reporting contacts") {
    PhysicsApp p(with_gravity(600.0f));
    floor_room(p.tiles());
    World &w = p.world();
    std::vector<Entity> pile;
    for (int i = 0; i < 24; ++i) {
        const float x = 40.0f + static_cast<float>((i * 37) % 80);
        const float y = 20.0f + 9.0f * static_cast<float>(i);
        const Collider c = i % 3 == 2 ? circle_collider(5) : box_collider({4.0f + static_cast<float>(i % 4), 5});
        pile.push_back(spawn_dynamic(w, {x, std::min(y, 140.0f)}, c, RigidBody{.restitution = i % 3 == 2 ? 0.5f : 0.1f, .friction = 0.5f}));
    }
    p.ticks(900);
    p.events().clear();
    p.ticks(120);
    float fastest = 0;
    for (const Entity e : pile) fastest = std::max(fastest, length(w.get<Velocity>(e).linear));
    CHECK(fastest < 15.0f);
    CHECK(p.events().started.size() <= 2);                       // at most a straggler settling
    CHECK(p.events().ended.size() <= 2);
}

TEST_CASE("bodies: layers and masks decide which bodies meet") {
    PhysicsApp p;
    World &w = p.world();
    const Entity a = spawn_kinematic(w, {0, 0}, box_collider({4, 4}, 1, 2));
    const Entity b = spawn_static(w, {6, 0}, box_collider({4, 4}, 2, 1));     // overlaps a: they meet
    const Entity c = spawn_static(w, {-6, 0}, box_collider({4, 4}, 4, 0xFFFFFFFFu)); // a's mask lacks 4
    const Entity d = spawn_static(w, {0, 6}, box_collider({4, 4}, 2, 4));     // d's mask lacks a's layer
    p.ticks(1);
    const auto &started = p.events().started;
    REQUIRE(started.size() == 1);
    CHECK(started[0].e.a == a);
    CHECK(started[0].e.b == b);
    (void)c;
    (void)d;
}

TEST_CASE("bodies: triggers report entering and leaving once and never push") {
    PhysicsApp p;
    World &w = p.world();
    Collider zone = box_collider({16, 16});
    zone.trigger = true;
    const Entity trigger = spawn_static(w, {100, 0}, zone);
    const Entity mover = spawn_kinematic(w, {40, 0}, box_collider({4, 4}), {120, 0});
    p.ticks(60);                                                 // x: 40 -> 160, through [84, 116]
    const auto &ev = p.events();
    REQUIRE(ev.entered.size() == 1);
    REQUIRE(ev.exited.size() == 1);
    CHECK(ev.entered[0].e.trigger == trigger);
    CHECK(ev.entered[0].e.other == mover);
    CHECK(ev.exited[0].e.trigger == trigger);
    CHECK(ev.exited[0].tick > ev.entered[0].tick);
    CHECK(ev.started.empty());
    CHECK(pos_of(w, mover).x == doctest::Approx(160.0f));
    // A dynamic body falls straight through a trigger.
    const Entity ball = spawn_dynamic(w, {100, -40}, circle_collider(3), {}, {0, 120});
    p.ticks(60);
    CHECK(pos_of(w, ball).y == doctest::Approx(80.0f));
}

TEST_CASE("bodies: CollisionStarted once per touch, CollisionEnded when apart, normal from a to b") {
    PhysicsApp p;
    World &w = p.world();
    const Entity left = spawn_kinematic(w, {0, 0}, box_collider({4, 4}), {60, 0});
    const Entity right = spawn_kinematic(w, {40, 0}, box_collider({4, 4}), {-60, 0});
    p.ticks(40);                                                 // they pass through each other
    const auto &ev = p.events();
    REQUIRE(ev.started.size() == 1);
    REQUIRE(ev.ended.size() == 1);
    CHECK(ev.started[0].e.a == left);
    CHECK(ev.started[0].e.b == right);
    CHECK(ev.started[0].e.normal.x == doctest::Approx(1.0f));   // from left to right
    CHECK(ev.ended[0].e.a == left);
    CHECK(ev.ended[0].tick > ev.started[0].tick);
}

TEST_CASE("bodies: a body resting flush on another stays in contact (one Started, no Ended)") {
    PhysicsApp p(with_gravity(600.0f));
    World &w = p.world();
    const Entity ground = spawn_static(w, {0, 100}, box_collider({64, 8}));
    const Entity crate = spawn_dynamic(w, {0, 60}, box_collider({6, 6}));
    p.ticks(180);
    CHECK(pos_of(w, crate).y + 6.0f == doctest::Approx(92.0f).epsilon(1e-3));
    const auto &ev = p.events();
    REQUIRE(ev.started.size() == 1);
    CHECK(ev.started[0].e.a == ground);
    CHECK(ev.started[0].e.normal.y == doctest::Approx(-1.0f));
    CHECK(ev.ended.empty());
}

TEST_CASE("bodies: one-way colliders hold bodies from above and let them up from below") {
    PhysicsApp p(with_gravity(600.0f));
    World &w = p.world();
    Collider plank = box_collider({32, 2});
    plank.one_way = true;
    spawn_static(w, {0, 100}, plank);                           // top at y = 98
    const Entity falling = spawn_dynamic(w, {-10, 60}, box_collider({4, 4}));
    const Entity rising = spawn_dynamic(w, {10, 130}, box_collider({4, 4}), {}, {0, -400});
    p.ticks(120);
    CHECK(pos_of(w, falling).y + 4.0f == doctest::Approx(98.0f).epsilon(1e-3));
    // The riser went up through the plank, then fell back onto it from above.
    CHECK(pos_of(w, rising).y + 4.0f == doctest::Approx(98.0f).epsilon(1e-3));
}

TEST_CASE("bodies: a sleeping body stays put until something moving touches it") {
    PhysicsApp p(with_gravity(600.0f));
    World &w = p.world();
    const Entity sleeper = spawn_dynamic(w, {0, 0}, box_collider({4, 4}), RigidBody{.sleeping = true});
    p.ticks(30);
    CHECK(pos_of(w, sleeper) == Vec2{0, 0});
    spawn_kinematic(w, {-30, 0}, box_collider({4, 4}), {120, 0});
    p.ticks(30);
    CHECK_FALSE(w.get<RigidBody>(sleeper).sleeping);
    CHECK(pos_of(w, sleeper).y > 0.0f);                         // awake: gravity again
}

TEST_CASE("bodies: undo restores state and contacts without spurious events") {
    PhysicsApp p(with_gravity(600.0f));
    World &w = p.world();
    spawn_static(w, {0, 100}, box_collider({64, 8}));
    const Entity crate = spawn_dynamic(w, {0, 60}, box_collider({6, 6}));
    Collider zone = box_collider({20, 20});
    zone.trigger = true;
    spawn_static(w, {0, 80}, zone);
    p.ticks(120);                                               // resting on the ground, inside the zone
    WorldSnapshot resting;
    w.snapshot_into(resting);
    const std::uint64_t resting_hash = w.state_hash();
    // Knock the crate away (out of contact and out of the zone), then undo.
    w.get<Transform>(crate).position = {200, 60};
    w.get<Velocity>(crate).linear = {};
    p.ticks(5);
    CHECK_FALSE(p.events().ended.empty());
    CHECK_FALSE(p.events().exited.empty());
    p.events().clear();
    w.restore(resting, RestoreOptions{.tick = true});
    CHECK(w.state_hash() == resting_hash);
    p.ticks(30);
    CHECK(p.events().started.empty());
    CHECK(p.events().ended.empty());
    CHECK(p.events().entered.empty());
    CHECK(p.events().exited.empty());

    // And the other way round: restoring a state where they are apart fires no Ended.
    w.get<Transform>(crate).position = {200, 60};
    p.ticks(60);
    WorldSnapshot apart;
    w.snapshot_into(apart);
    w.get<Transform>(crate).position = {0, 86};
    p.ticks(3);
    p.events().clear();
    w.restore(apart, RestoreOptions{.tick = true});
    p.ticks(10);
    CHECK(p.events().total() == 0);
}

TEST_CASE("bodies: the same inputs give the same state hash, also after a save and restore") {
    const auto run = [](int ticks, WorldSnapshot *mid, int snap_at) {
        PhysicsApp p(with_gravity(400.0f));
        floor_room(p.tiles());
        World &w = p.world();
        Rng rng(1234);
        for (int i = 0; i < 60; ++i) {
            const Vec2 at{24.0f + rng.range(0.0f, 260.0f), 16.0f + rng.range(0.0f, 120.0f)};
            const Collider c = i % 2 == 0 ? box_collider({rng.range(2.0f, 6.0f), rng.range(2.0f, 6.0f)})
                                          : circle_collider(rng.range(2.0f, 5.0f));
            spawn_dynamic(w, at, c, RigidBody{.mass = rng.range(0.5f, 3.0f), .restitution = rng.range(0.0f, 0.5f)},
                          {rng.range(-80.0f, 80.0f), rng.range(-80.0f, 80.0f)});
        }
        std::vector<std::uint64_t> hashes;
        for (int t = 0; t < ticks; ++t) {
            if (mid != nullptr && t == snap_at) w.snapshot_into(*mid);
            p.ticks(1);
            hashes.push_back(w.state_hash());
        }
        return hashes;
    };
    WorldSnapshot snap;
    const std::vector<std::uint64_t> a = run(200, &snap, 100);
    const std::vector<std::uint64_t> b = run(200, nullptr, 0);
    CHECK(a == b);

    // Continue from the snapshot in a fresh app: the second half hashes the same.
    PhysicsApp p(with_gravity(400.0f));
    floor_room(p.tiles());
    World &w = p.world();
    w.restore(snap);
    for (int t = 100; t < 200; ++t) {
        p.ticks(1);
        CHECK(w.state_hash() == a[static_cast<std::size_t>(t)]);
    }
}

TEST_CASE("bodies: PhysicsStats counts bodies, pairs, cells and contacts; overflow keeps the lowest pairs") {
    PhysicsConfig cfg;
    cfg.pair_capacity = 2;
    PhysicsApp p(cfg);
    World &w = p.world();
    std::vector<Entity> pile;
    for (int i = 0; i < 4; ++i) pile.push_back(spawn_kinematic(w, {static_cast<float>(i), 0}, box_collider({8, 8})));
    spawn_static(w, {500, 500}, box_collider({4, 4}));
    p.ticks(1);
    const PhysicsStats &s = p.stats();
    CHECK(s.bodies == 5);
    CHECK(s.pairs == 2);
    CHECK(s.dropped_pairs == 4);                                 // 6 overlapping pairs, 2 kept
    CHECK(s.cells >= 2);
    CHECK(s.contacts == 2);
    // The kept pairs are the first two in (a.index, b.index) order: (0,1) and (0,2).
    const auto &started = p.events().started;
    REQUIRE(started.size() == 2);
    CHECK(started[0].e.a == pile[0]);
    CHECK(started[0].e.b == pile[1]);
    CHECK(started[1].e.a == pile[0]);
    CHECK(started[1].e.b == pile[2]);
}

TEST_CASE("bodies: polygons collide by their shape (SAT)") {
    PhysicsApp p(with_gravity(600.0f));
    World &w = p.world();
    const Vec2 tri[3] = {{-16, 8}, {16, 8}, {0, -8}};
    const std::uint16_t id = w.resources().get<PolygonShapes>().add(tri);
    Collider c;
    c.shape = ShapeType::Polygon;
    c.polygon = id;
    spawn_static(w, {0, 100}, c);                                // apex at (0, 92)
    const Entity ball = spawn_dynamic(w, {0, 60}, circle_collider(4));
    p.ticks(90);
    // The ball rests on (or rolls off) the triangle, never inside it.
    const Vec2 at = pos_of(w, ball);
    CHECK(at.y < 92.0f);
    CHECK(p.events().started.size() >= 1);
}

TEST_CASE("bodies: steady-state ticks do not allocate") {
    PhysicsApp p(with_gravity(500.0f));
    floor_room(p.tiles());
    World &w = p.world();
    p.events().enabled = false;
    Rng rng(99);
    for (int i = 0; i < 150; ++i) {
        const Vec2 at{24.0f + rng.range(0.0f, 260.0f), 16.0f + rng.range(0.0f, 120.0f)};
        if (i % 3 == 0) {
            spawn_static(w, at, box_collider({4, 4}));
        } else if (i % 3 == 1) {
            spawn_dynamic(w, at, circle_collider(3), RigidBody{.restitution = 0.3f}, {rng.range(-50.0f, 50.0f), 0});
        } else {
            spawn_kinematic(w, at, box_collider({3, 3}), {rng.range(-20.0f, 20.0f), 0});
        }
    }
    Collider zone = box_collider({40, 40});
    zone.trigger = true;
    spawn_static(w, {150, 100}, zone);
    lam::test::run_ticks(p.app(), 120);
    CHECK_NO_ALLOC(lam::test::run_ticks(p.app(), 600));
}

TEST_CASE("bodies: draw_colliders outlines every collider when show_colliders is on (F4)") {
    PhysicsApp p;
    World &w = p.world();
    spawn_static(w, {0, 0}, box_collider({4, 4}));
    spawn_dynamic(w, {40, 0}, circle_collider(3));
    Collider zone = box_collider({8, 8});
    zone.trigger = true;
    spawn_static(w, {80, 0}, zone);
    const std::array<Vec2, 3> tri{Vec2{0, -4}, Vec2{-4, 4}, Vec2{4, 4}};
    Collider poly = box_collider({4, 4});
    poly.shape = ShapeType::Polygon;
    poly.polygon = w.resources().get<PolygonShapes>().add(tri);
    spawn_static(w, {120, 0}, poly);
    const Entity bullet = w.create();
    w.add<Transform>(bullet, Transform{{160, 0}});
    w.add<Velocity>(bullet);
    w.add<Projectile>(bullet, Projectile{.radius = 2});
    DebugDraw &dd = w.resources().get<DebugDraw>();
    p.app().run_frames(2);
    CHECK(dd.prims().empty());                                   // off by default
    p.app().res<DebugOverlay>().show_colliders = true;
    dd.clear();
    p.app().run_frames(1);
    if constexpr (LAM_DEV) {
        // A box, a circle, a trigger box, three polygon edges and the projectile's circle.
        std::size_t rects = 0, circles = 0, lines = 0;
        for (const DebugPrim &prim : dd.prims()) {
            rects += prim.kind == DebugPrimKind::Rect ? 1u : 0u;
            circles += prim.kind == DebugPrimKind::Circle ? 1u : 0u;
            lines += prim.kind == DebugPrimKind::Line ? 1u : 0u;
        }
        CHECK(rects == 2);
        CHECK(circles == 2);
        CHECK(lines == 3);
    } else {
        CHECK(dd.prims().empty());                               // DebugDraw is a no-op without LAM_DEV
    }
}
