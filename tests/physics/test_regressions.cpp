// Regression tests for lam_physics. Every case failed before its fix.
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <lam/core/data.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/test/support.h>

#include "detail/physics_world.h"
#include "detail/shapes.h"
#include "detail/tile_math.h"
#include "physics_app.h"
#include "physics_test_util.h"

using namespace lam;
using lam::test::box_collider;
using lam::test::grid_from;
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

Entity spawn_platformer(World &w, Vec2 pos, Vec2 half = {5, 7}, Vec2 offset = {}) {
    const Entity e = w.create();
    w.add<Transform>(e, Transform{pos});
    Collider c = box_collider(half);
    c.offset = offset;
    w.add<Collider>(e, c);
    w.add<PlatformerMotor>(e);
    return e;
}

Entity spawn_bullet(World &w, Vec2 pos, Vec2 vel, Projectile p = {}) {
    const Entity e = w.create();
    w.add<Transform>(e, Transform{pos});
    w.add<Velocity>(e, Velocity{vel});
    w.add<Projectile>(e, p);
    return e;
}

Rect box_of(World &w, Entity e) { return collider_box(w.get<Collider>(e), pos_of(w, e)); }

float bottom_of(World &w, Entity e) {
    const Rect r = box_of(w, e);
    return r.y + r.h;
}

// A lift: PathFollower + MovingPlatform + Velocity, from `from` to `to` at `speed`.
Entity spawn_lift(World &w, Vec2 from, Vec2 to, float speed, Vec2 half) {
    const Entity e = spawn_static(w, from, box_collider(half));
    w.add<MovingPlatform>(e);
    PathFollower path;
    path.points[0] = from;
    path.points[1] = to;
    path.count = 2;
    path.speed = speed;
    path.ping_pong = false;
    w.add<PathFollower>(e, path);
    w.add<Velocity>(e);
    return e;
}

} // namespace

// ---- physics-core-1 ---------------------------------------------------------------------------

TEST_CASE("a box falling past a ramp's low end lands on the Solid under the slope") {
    // A ramp descending to the right in column 3 (row 5) over Solid rows 6-8; floor on row 9.
    const TileWorld tw = grid_from({
        "........",
        "........",
        "........",
        "........",
        "........",
        "###\\....",
        "####....",
        "####....",
        "####....",
        "########",
    });
    // Foot (centre x 64.5) just past the slope, left 5.5 px over column 3, bottom at the slope's
    // low end: the fall must stop on the Solid's top (96), not sink into it.
    MoveBoxOptions o;
    o.platformer = true;
    BoxMove r = move_box(tw, Rect{58.5f, 80.0f, 12.0f, 16.0f}, {0.0f, 10.0f}, o);
    CHECK(r.box.y + r.box.h == doctest::Approx(96.0f));
    CHECK(r.box.y + r.box.h <= 96.0f);
    CHECK(r.grounded);
    // And it is free to walk on.
    const BoxMove next = move_box(tw, r.box, {5.0f, 0.0f}, o);
    CHECK_FALSE(next.hit_x);
    CHECK(next.box.x == doctest::Approx(r.box.x + 5.0f));

    // The same through the motor: walk down the ramp, let go just past its low end.
    PhysicsApp p;
    p.tiles() = tw;
    World &w = p.world();
    const Entity hero = spawn_platformer(w, {24, 72}, {6, 8});
    for (int i = 0; i < 30 && !w.get<PlatformerMotor>(hero).on_ground; ++i) p.ticks(1);
    REQUIRE(w.get<PlatformerMotor>(hero).on_ground);
    w.get<PlatformerMotor>(hero).move_x = 0.3f;
    for (int i = 0; i < 600 && pos_of(w, hero).x < 64.5f; ++i) p.ticks(1);
    w.get<PlatformerMotor>(hero).move_x = 0.0f;
    p.ticks(30);
    CHECK(w.get<PlatformerMotor>(hero).on_ground);
    // Holding right takes it off the ledge and down to the floor (y = 144).
    w.get<PlatformerMotor>(hero).move_x = 1.0f;
    const float x0 = pos_of(w, hero).x;
    p.ticks(40);
    CHECK(pos_of(w, hero).x > x0 + 20.0f);
    CHECK(bottom_of(w, hero) == doctest::Approx(144.0f));
}

// ---- physics-core-2 / physics-motors-3 ---------------------------------------------------------

TEST_CASE("a restore keeps the previous-contact set of the run that was saved") {
    SUBCASE("a trigger spawned over a body between ticks still reports TriggerEntered") {
        PhysicsApp p;
        World &w = p.world();
        spawn_kinematic(w, {0, 0}, box_collider({4, 4}));
        p.ticks(3);
        Collider zone = box_collider({16, 16});
        zone.trigger = true;
        spawn_static(w, {0, 0}, zone);                          // e.g. a PostPhysics explosion
        WorldSnapshot snap;
        w.snapshot_into(snap);
        p.events().clear();
        p.ticks(1);
        CHECK(p.events().entered.size() == 1);
        // Undo in the same world, and a load into a fresh app: the same events.
        p.events().clear();
        w.restore(snap, RestoreOptions{.tick = true});
        p.ticks(1);
        CHECK(p.events().entered.size() == 1);
        PhysicsApp q;
        q.world().restore(snap);
        q.ticks(1);
        CHECK(q.events().entered.size() == 1);
    }
    SUBCASE("a body destroyed between ticks still reports CollisionEnded") {
        PhysicsApp p;
        World &w = p.world();
        spawn_kinematic(w, {0, 0}, box_collider({4, 4}));
        const Entity wall = spawn_static(w, {8, 0}, box_collider({4, 4}));   // flush: touching
        p.ticks(3);
        REQUIRE(p.events().started.size() == 1);
        w.destroy(wall);                                         // e.g. a Lifetime at TickEnd
        WorldSnapshot snap;
        w.snapshot_into(snap);
        p.events().clear();
        p.ticks(1);
        CHECK(p.events().ended.size() == 1);
        p.events().clear();
        w.restore(snap, RestoreOptions{.tick = true});
        p.ticks(1);
        CHECK(p.events().ended.size() == 1);
        PhysicsApp q;
        q.world().restore(snap);
        q.ticks(1);
        CHECK(q.events().ended.size() == 1);
    }
}

TEST_CASE("the previous-contact set is a snapshot and save resource that round-trips") {
    detail::PhysicsContactSet set;
    set.records.push_back(detail::ContactRecord{Entity{3, 1}, Entity{7, 2}, {}, 0});
    set.records.push_back(detail::ContactRecord{Entity{4, 5}, Entity{2, 9}, {}, 1});
    DataWriter out;
    lam_write(set, out, SerializeContext{});
    Result<std::string> text = out.finish();
    REQUIRE(text.ok());
    Result<DataDocument> doc = DataDocument::parse(*text, "contacts.json");
    REQUIRE(doc.ok());
    detail::PhysicsContactSet back;
    DataReader root = doc->root();
    REQUIRE(lam_read(back, root, SerializeContext{}).ok());
    CHECK(doc->finish().ok());
    REQUIRE(back.records.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(back.records[i].first == set.records[i].first);
        CHECK(back.records[i].second == set.records[i].second);
        CHECK(back.records[i].kind == set.records[i].kind);
    }
    // Copy-assignment (how World::restore puts it back) marks it restored; the default is not.
    CHECK_FALSE(back.restored);
    detail::PhysicsContactSet live;
    live = back;
    CHECK(live.restored);
    // Registered for snapshots and saves, not for the state hash.
    const ComponentInfo &info = ComponentRegistry::global().info<detail::PhysicsContactSet>();
    CHECK(info.name == "PhysicsContactSet");
    CHECK((info.flags & kSerialize) != 0);
    CHECK((info.flags & kSnapshot) != 0);
    CHECK((info.flags & kHash) == 0);
}

// ---- physics-core-3 / physics-motors-4 ---------------------------------------------------------

TEST_CASE("a carried rider is blocked by a solid spawned since the last tick") {
    const auto setup = [](PhysicsApp &p, Entity &rider, Entity &block) {
        World &w = p.world();
        const Entity platform = spawn_kinematic(w, {100, 100}, box_collider({32, 4}), {30, 0});   // top 96
        w.add<MovingPlatform>(platform);
        rider = spawn_platformer(w, {100, 88});
        for (int i = 0; i < 30 && w.get<PlatformerMotor>(rider).riding != platform; ++i) p.ticks(1);
        REQUIRE(w.get<PlatformerMotor>(rider).riding == platform);
        // Between ticks game code spawns a kinematic block 0.25 px ahead of the rider.
        const Rect r = box_of(w, rider);
        block = spawn_kinematic(w, {r.x + r.w + 0.25f + 4.0f, r.y + r.h * 0.5f}, box_collider({4, 6}));
    };
    PhysicsApp p;
    Entity rider, block;
    setup(p, rider, block);
    World &w = p.world();
    WorldSnapshot snap;
    w.snapshot_into(snap);
    const float block_left = box_of(w, block).x;
    std::vector<std::uint64_t> hashes;
    for (int i = 0; i < 40; ++i) {
        p.ticks(1);
        hashes.push_back(w.state_hash());
        CHECK(box_of(w, rider).x + box_of(w, rider).w <= block_left);
    }
    // A load of the same snapshot takes the same path.
    PhysicsApp q;
    q.world().restore(snap);
    for (int i = 0; i < 40; ++i) {
        q.ticks(1);
        CHECK(q.world().state_hash() == hashes[static_cast<std::size_t>(i)]);
    }
}

// ---- physics-core-4 / physics-motors-5 ---------------------------------------------------------

TEST_CASE("a projectile stopped by a tile reports the hit once, whatever the rounding") {
    for (int k = 0; k < 40; ++k) {
        CAPTURE(k);
        PhysicsApp p;
        p.tiles() = grid_from({"....#....", ".........", "........."});
        World &w = p.world();
        const float speed = 1234.0f + 3.7f * static_cast<float>(k);
        const Entity e = spawn_bullet(w, {8.0f + 0.13f * static_cast<float>(k), 8.1f}, {speed, 0},
                                      Projectile{.radius = 1.3f});
        p.ticks(10);
        CHECK(p.events().hits.size() == 1);
        CHECK(w.get<Velocity>(e).linear == Vec2{});
    }
    SUBCASE("a diagonal shot") {
        PhysicsApp p;
        p.tiles() = grid_from({"........", "........", ".....#..", ".....#..", ".....#..", "........"});
        World &w = p.world();
        spawn_bullet(w, {20, 20}, {3000, 2100}, Projectile{.radius = 1.7f});   // hits x = 78.3 on tick 2
        p.ticks(32);
        CHECK(p.events().hits.size() == 1);
    }
    SUBCASE("created inside a Solid tile: nothing every tick") {
        PhysicsApp p;
        p.tiles() = grid_from({"....#....", ".........", "........."});
        World &w = p.world();
        spawn_bullet(w, {72, 8}, {0, 0}, Projectile{.radius = 1});
        spawn_bullet(w, {70, 8}, {60, 0}, Projectile{.radius = 1});
        p.ticks(10);
        CHECK(p.events().hits.empty());
    }
    SUBCASE("bounced by the game off the wall it rests against, it leaves") {
        PhysicsApp p;
        p.tiles() = grid_from({"....#....", ".........", "........."});
        World &w = p.world();
        p.app().add_system(Stage::PostPhysics, [](Ctx &ctx) {
            for (const ProjectileHit &h : ctx.read<ProjectileHit>()) {
                if (h.tile) ctx.world().get<Velocity>(h.projectile).linear = {-600, 0};
            }
        }, "test.bounce");
        const Entity e = spawn_bullet(w, {8, 8}, {1234, 0}, Projectile{.radius = 1.3f});
        p.ticks(6);                                              // hit on tick 3, then 10 px a tick
        CHECK(p.events().hits.size() == 1);
        CHECK(pos_of(w, e).x < 40.0f);
    }
}

// ---- physics-core-5 ---------------------------------------------------------------------------

TEST_CASE("a fast platformer move is not cancelled by a platform above the head") {
    const TileWorld tw = grid_from({
        "..........",
        "..........",
        "..........",
        "......##..",
        "..........",
        "..........",
        "##########",
    });
    MoveBoxOptions o;
    o.platformer = true;
    const Rect box{40, 64, 32, 32};                              // standing on row 6 (y = 96); step 16
    const BoxMove r = move_box(tw, box, {60, 0}, o);
    CHECK_FALSE(r.hit_x);
    CHECK(r.box.x == doctest::Approx(100.0f));
    CHECK(r.box.y == doctest::Approx(64.0f));
    CHECK(r.grounded);
    // A real step up is still taken (a one-tile ledge at the end of a long move).
    const TileWorld ledge = grid_from({
        "..........",
        "..........",
        "..........",
        "..........",
        "..........",
        "......####",
        "##########",
    });
    const BoxMove up = move_box(ledge, box, {60, 0}, o);
    CHECK_FALSE(up.hit_x);
    CHECK(up.box.x == doctest::Approx(100.0f));
    CHECK(up.box.y + up.box.h == doctest::Approx(80.0f));
}

// ---- physics-core-6 ---------------------------------------------------------------------------

TEST_CASE("a fast dynamic body lands on a thin platform instead of tunnelling") {
    // Drops of 400 to 1100 px (14 to 24 px a tick at impact) onto a 4 px plank.
    for (const bool one_way : {false, true}) {
        int lost = 0;
        for (int k = 0; k < 24; ++k) {
            PhysicsApp p(with_gravity(900.0f));
            World &w = p.world();
            Collider plank = box_collider({32, 2});
            plank.one_way = one_way;
            spawn_static(w, {0, 1520}, plank);                   // top 1518
            const float drop = 400.0f + 30.0f * static_cast<float>(k);
            const Entity crate = spawn_dynamic(w, {0, 1518.0f - drop - 8.0f}, box_collider({8, 8}));
            p.ticks(150);
            if (!(std::fabs(bottom_of(w, crate) - 1518.0f) < 0.05f)) ++lost;
        }
        CAPTURE(one_way);
        CHECK(lost == 0);
    }
    SUBCASE("a body that came from above lands on a one-way platform's corner") {
        PhysicsApp p(with_gravity(900.0f));
        World &w = p.world();
        Collider plank = box_collider({32, 2});
        plank.one_way = true;
        spawn_static(w, {0, 420}, plank);
        // 0.25 px of horizontal overlap with the plank's right end.
        const Entity crate = spawn_dynamic(w, {32.0f + 8.0f - 0.25f, 300}, box_collider({8, 8}));
        p.ticks(90);
        CHECK(bottom_of(w, crate) == doctest::Approx(418.0f).epsilon(1e-3));
    }
}

// ---- physics-core-7 ---------------------------------------------------------------------------

TEST_CASE("a flush polygon contact counts as touching whatever the spawn order") {
    for (const bool polygon_first : {true, false}) {
        CAPTURE(polygon_first);
        PhysicsApp p;
        World &w = p.world();
        const std::array<Vec2, 4> square{Vec2{-8, -8}, Vec2{8, -8}, Vec2{8, 8}, Vec2{-8, 8}};
        const std::uint16_t id = w.resources().get<PolygonShapes>().add(square);
        Collider poly;
        poly.shape = ShapeType::Polygon;
        poly.polygon = id;
        const auto spawn_poly = [&] { spawn_kinematic(w, {0, -16}, poly); };   // bottom -8: flush
        const auto spawn_box = [&] { spawn_static(w, {0, 0}, box_collider({8, 8})); };
        if (polygon_first) {
            spawn_poly();
            spawn_box();
        } else {
            spawn_box();
            spawn_poly();
        }
        p.ticks(3);
        CHECK(p.events().started.size() == 1);
        CHECK(p.events().ended.empty());
    }
    // The shape test itself is symmetric (polygon/box and polygon/circle).
    const std::array<Vec2, 4> square{Vec2{-8, -8}, Vec2{8, -8}, Vec2{8, 8}, Vec2{-8, 8}};
    PolygonShapes shapes;
    Collider poly;
    poly.shape = ShapeType::Polygon;
    poly.polygon = shapes.add(square);
    const detail::ShapeView sp = detail::make_shape(poly, {0, -16}, &shapes);
    const detail::ShapeView sb = detail::make_shape(box_collider({8, 8}), {0, 0}, &shapes);
    Collider circle;
    circle.shape = ShapeType::Circle;
    circle.radius = 8;
    const detail::ShapeView sc = detail::make_shape(circle, {16, -16}, &shapes);
    detail::ContactInfo ci;
    CHECK(detail::collide(sp, sb, ci, detail::kContactSkin));
    CHECK(detail::collide(sb, sp, ci, detail::kContactSkin));
    CHECK(detail::collide(sp, sc, ci, detail::kContactSkin));
    CHECK(detail::collide(sc, sp, ci, detail::kContactSkin));
    CHECK_FALSE(detail::collide(sp, sb, ci, 0.0f));
    CHECK_FALSE(detail::collide(sb, sp, ci, 0.0f));
}

// ---- physics-core-8 ---------------------------------------------------------------------------

TEST_CASE("hazard tiles test a polygon body by its real bounds") {
    PhysicsApp p;
    p.tiles() = grid_from({
        "........",
        "........",
        "......~.",
        "........",
    });
    World &w = p.world();
    const std::array<Vec2, 4> plank{Vec2{-32, -4}, Vec2{32, -4}, Vec2{32, 4}, Vec2{-32, 4}};
    Collider c;
    c.shape = ShapeType::Polygon;
    c.polygon = w.resources().get<PolygonShapes>().add(plank);
    const Entity e = spawn_kinematic(w, {72, 40}, c);           // x 40..104; hazard at x 96..112
    p.ticks(1);
    REQUIRE(p.events().hazards.size() == 1);
    CHECK(p.events().hazards[0].e.entity == e);
    CHECK(p.events().hazards[0].e.tile == IVec2{6, 2});
}

// ---- physics-core-9 / physics-motors-10 ---------------------------------------------------------

TEST_CASE("huge platformer falls land on the floor (no float-to-int overflow)") {
    const TileWorld tw = grid_from({
        "......",
        "......",
        "......",
        "......",
        "######",
    });
    MoveBoxOptions o;
    o.platformer = true;
    for (const float dy : {1e11f, 4e10f, 3e38f}) {
        CAPTURE(dy);
        const BoxMove r = move_box(tw, Rect{40, 10, 12, 16}, {0, dy}, o);
        CHECK(r.grounded);
        CHECK(r.box.y + r.box.h == doctest::Approx(64.0f));
    }
}

// ---- physics-core-10 ---------------------------------------------------------------------------

TEST_CASE("a huge sweep radius returns at once") {
    TileWorld tw = grid_from({"........", "........", "........", "........"});
    for (const float r : {1e6f, 1e11f, 3e38f}) {
        CAPTURE(r);
        const std::optional<detail::TileSweepHit> h = detail::sweep_circle_tiles(tw, {40, 30}, {60, 30}, r);
        REQUIRE(h.has_value());
        CHECK(h->t == 0.0f);
    }
    tw.outside_solid = false;
    CHECK_FALSE(detail::sweep_circle_tiles(tw, {40, 30}, {60, 30}, 1e6f).has_value());
    // Through a projectile, too.
    PhysicsApp p;
    p.tiles() = grid_from({"........", "........", "........", "........"});
    spawn_bullet(p.world(), {40, 30}, {600, 0}, Projectile{.radius = 1e6f});
    p.ticks(1);
    CHECK(p.events().hits.size() <= 1);
}

// ---- physics-motors-1 ---------------------------------------------------------------------------

TEST_CASE("riders stay on rising lifts with non-dyadic extents") {
    for (int k = 0; k < 12; ++k) {
        const float speed = 20.0f + 6.0f * static_cast<float>(k);
        CAPTURE(speed);
        PhysicsApp p;
        World &w = p.world();
        // The lift waits (speed 0) until the rider stands on it, then rises.
        const Entity lift = spawn_lift(w, {100, 360}, {100, -40}, 0.0f, {24, 4.1f});
        const Entity rider = spawn_platformer(w, {100, 340}, {5, 7}, {0, -7});
        for (int i = 0; i < 60 && w.get<PlatformerMotor>(rider).riding != lift; ++i) p.ticks(1);
        REQUIRE(w.get<PlatformerMotor>(rider).riding == lift);
        w.get<PathFollower>(lift).speed = speed;
        int lost = 0;
        for (int i = 0; i < 1200 && pos_of(w, lift).y > -30.0f; ++i) {
            p.ticks(1);
            if (w.get<PlatformerMotor>(rider).riding != lift) ++lost;
        }
        CHECK(lost == 0);
        CHECK(bottom_of(w, rider) <= box_of(w, lift).y);
        CHECK(bottom_of(w, rider) == doctest::Approx(box_of(w, lift).y).epsilon(1e-4));
    }
}

// ---- physics-motors-2 ---------------------------------------------------------------------------

TEST_CASE("a target that moves onto a flying projectile is hit") {
    int missed = 0;
    for (int k = 0; k < 50; ++k) {
        PhysicsApp p;
        World &w = p.world();
        const Entity target = spawn_kinematic(w, {300.0f + 0.37f * static_cast<float>(k), 0}, box_collider({6, 6}),
                                              {-120, 0});
        spawn_bullet(w, {0, 0}, {600, 0}, Projectile{.radius = 1});
        p.ticks(60);
        int hits = 0;
        for (const auto &h : p.events().hits) hits += h.e.target == target ? 1 : 0;
        if (hits != 1) ++missed;
    }
    CHECK(missed == 0);
}

// ---- physics-motors-6 ---------------------------------------------------------------------------

TEST_CASE("a rising lift catches a body falling onto it") {
    int fell_through = 0;
    for (int k = 0; k < 60; ++k) {
        PhysicsApp p;
        World &w = p.world();
        const Entity lift = spawn_lift(w, {100, 400}, {100, 0}, 60.0f, {24, 4});   // top 396, 1 px/tick
        const float drop = 20.0f + 0.37f * static_cast<float>(k);
        const Entity body = spawn_platformer(w, {100, 396.0f - drop - 7.0f});
        p.ticks(60);
        if (!(bottom_of(w, body) <= box_of(w, lift).y + 0.01f) || w.get<PlatformerMotor>(body).riding != lift) {
            ++fell_through;
        }
    }
    CHECK(fell_through == 0);
}

// ---- physics-motors-7 ---------------------------------------------------------------------------

TEST_CASE("a platform carries its rider onto a ramp by platformer rules") {
    PhysicsApp p;
    p.tiles() = grid_from({
        "....................",
        "....................",
        "....................",
        "....................",
        "....................",
        "....................",
        "....................",
        "...............#####",
        "..............//####",
        ".............//#####",
        "####################",
        "####################",
    });
    World &w = p.world();
    // A conveyor-like platform sunk into the floor (top at y = 160) moving right at 40 px/s.
    const Entity platform = spawn_kinematic(w, {60, 168}, box_collider({48, 8}), {40, 0});
    w.add<MovingPlatform>(platform);
    const Entity rider = spawn_platformer(w, {100, 150});
    for (int i = 0; i < 30 && w.get<PlatformerMotor>(rider).riding != platform; ++i) p.ticks(1);
    REQUIRE(w.get<PlatformerMotor>(rider).riding == platform);
    p.ticks(240);
    // The first slope tile starts at x = 208: the rider got onto the ramp, not stopped at it.
    CHECK(box_of(w, rider).x + box_of(w, rider).w > 208.0f);
    CHECK(w.get<PlatformerMotor>(rider).on_ground);
}

// ---- physics-motors-8 ---------------------------------------------------------------------------

TEST_CASE("stepping up onto a ledge sees an entity ceiling above the head") {
    PhysicsApp p;
    p.tiles() = grid_from({
        "....................",
        "....................",
        "....................",
        "....................",
        "....................",
        "....................",
        "....................",
        "....................",
        "....................",
        "........############",
        "####################",
        "####################",
    });
    World &w = p.world();
    const Entity hero = spawn_platformer(w, {100, 144}, {16, 16});   // bottom 160, top 128
    for (int i = 0; i < 30 && !w.get<PlatformerMotor>(hero).on_ground; ++i) p.ticks(1);
    REQUIRE(w.get<PlatformerMotor>(hero).on_ground);
    // Over the ledge's edge, its bottom 8 px above the hero's head: no room to step up.
    const Entity solid = spawn_static(w, {145, 110}, box_collider({25, 10}));   // x 120..170, bottom 120
    w.get<PlatformerMotor>(hero).move_x = 1.0f;
    for (int i = 0; i < 60; ++i) {
        p.ticks(1);
        CHECK_FALSE(box_of(w, hero).overlaps(box_of(w, solid)));
    }
    CHECK(bottom_of(w, hero) == doctest::Approx(160.0f));
    CHECK(w.get<PlatformerMotor>(hero).on_wall_right);
}

// ---- physics-motors-9 ---------------------------------------------------------------------------

TEST_CASE("a projectile finds a collider projectile moved earlier in the same sweep") {
    PhysicsApp p;
    World &w = p.world();
    // A missile with a collider (a body) that hits nothing, crossing a 64 px cell this tick.
    const Entity missile = spawn_bullet(w, {10, 0}, {6000, 0}, Projectile{.radius = 1, .mask = 0});
    w.add<Collider>(missile, box_collider({4, 4}));
    // An interceptor later in dense order, sweeping down x = 110.
    const Entity interceptor = spawn_bullet(w, {110, -50}, {0, 6000}, Projectile{.radius = 1});
    p.ticks(1);
    REQUIRE(p.events().hits.size() == 1);
    CHECK(p.events().hits[0].e.projectile == interceptor);
    CHECK(p.events().hits[0].e.target == missile);
}
