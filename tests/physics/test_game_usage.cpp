// lam_physics behaviour that games rely on.
#include <array>
#include <vector>

#include <lam/ecs/lifetime.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/physics/steering.h>
#include <lam/test/support.h>

#include "physics_app.h"
#include "physics_test_util.h"

using namespace lam;
using lam::test::box_collider;
using lam::test::circle_collider;
using lam::test::grid_from;
using lam::test::PhysicsApp;
using lam::test::pos_of;
using lam::test::spawn_dynamic;
using lam::test::spawn_kinematic;
using lam::test::spawn_static;

namespace {

Entity spawn_top_down(World &w, Vec2 pos, CollisionMask solid_mask, Vec2 half = {5, 5}) {
    const Entity e = w.create();
    w.add<Transform>(e, Transform{pos});
    w.add<Collider>(e, box_collider(half));
    w.add<TopDownMotor>(e, TopDownMotor{.solid_mask = solid_mask});
    return e;
}

Entity spawn_platformer(World &w, Vec2 pos, Vec2 half = {5, 7}) {
    const Entity e = w.create();
    w.add<Transform>(e, Transform{pos});
    w.add<Collider>(e, box_collider(half));
    w.add<PlatformerMotor>(e);
    return e;
}

} // namespace

// ---- Pair rule, kinematic contacts, helpers ----------------------------------------------------

TEST_CASE("the layer/mask test runs both ways") {
    PhysicsApp p;
    World &w = p.world();
    // a is on layer 1 and reports layer 2; b is on layer 2 but does not report layer 1.
    spawn_kinematic(w, {0, 0}, box_collider({4, 4}, 1, 2));
    spawn_static(w, {6, 0}, box_collider({4, 4}, 2, 4));
    p.ticks(2);
    CHECK(p.events().started.empty());
    // Both ways: they meet.
    spawn_static(w, {0, 6}, box_collider({4, 4}, 2, 1));
    p.ticks(1);
    CHECK(p.events().started.size() == 1);
}

TEST_CASE("two kinematic motor bodies that do not block each other still report contact") {
    PhysicsApp p;
    World &w = p.world();
    // solid_mask 0: neither blocks the other (tiles only), so they pass through each other.
    const Entity left = spawn_top_down(w, {0, 0}, 0);
    const Entity right = spawn_top_down(w, {40, 0}, 0);
    w.get<TopDownMotor>(left).move = {1, 0};
    w.get<TopDownMotor>(right).move = {-1, 0};
    p.ticks(40);
    const auto &started = p.events().started;
    REQUIRE(started.size() == 1);
    const CollisionStarted &c = started[0].e;
    CHECK(c.a == left);                                          // a.index < b.index
    CHECK(c.b == right);
    // other() and normal_from() read the event from either side without a swap.
    CHECK(other(c, left) == right);
    CHECK(other(c, right) == left);
    CHECK(other(c, Entity{99, 1}) == Entity{});
    CHECK(normal_from(c, left).x > 0.9f);                        // from left toward right
    CHECK(normal_from(c, right).x < -0.9f);
    CHECK(normal_from(c, Entity{99, 1}) == Vec2{});
    // They walked through each other (no blocking) and finally parted.
    CHECK(pos_of(w, left).x > pos_of(w, right).x);
    REQUIRE(p.events().ended.size() == 1);
    CHECK(other(p.events().ended[0].e, right) == left);
    // Static-Static pairs never report anything.
    spawn_static(w, {200, 200}, box_collider({4, 4}));
    spawn_static(w, {202, 200}, box_collider({4, 4}));
    p.events().clear();
    p.ticks(3);
    CHECK(p.events().total() == 0);
}

TEST_CASE("two overlapping triggers each report their own TriggerEntered") {
    PhysicsApp p;
    World &w = p.world();
    Collider zone = box_collider({8, 8});
    zone.trigger = true;
    const Entity a = spawn_static(w, {0, 0}, zone);
    const Entity b = spawn_kinematic(w, {40, 0}, zone, {-600, 0});
    p.ticks(3);
    const auto &entered = p.events().entered;
    REQUIRE(entered.size() == 2);
    CHECK(entered[0].e.trigger == a);
    CHECK(entered[0].e.other == b);
    CHECK(entered[1].e.trigger == b);
    CHECK(entered[1].e.other == a);
    CHECK(p.events().started.empty());
}

TEST_CASE("motor wall flags count entity solids as well as tiles") {
    PhysicsApp p;
    World &w = p.world();
    p.tiles() = grid_from({
        "#................",
        "#................",
        "#................",
        "#................",
        "#................",
        "#................",
        "#................",
        "#################",
    });
    const Entity hero = spawn_platformer(w, {60, 100});
    spawn_static(w, {100, 90}, box_collider({8, 22}));           // a crate wall, layer 1
    w.get<PlatformerMotor>(hero).move_x = 1;
    p.ticks(90);
    const PlatformerMotor &m = w.get<PlatformerMotor>(hero);
    CHECK(m.on_wall_right);
    CHECK(pos_of(w, hero).x == doctest::Approx(87.0f));          // flush against x = 92
    w.get<PlatformerMotor>(hero).move_x = -1;
    p.ticks(120);
    CHECK(w.get<PlatformerMotor>(hero).on_wall_left);            // the tile wall at x = 16
    CHECK(pos_of(w, hero).x == doctest::Approx(21.0f));
}

// ---- Motor intents ------------------------------------------------------------------------------

TEST_CASE("motors never clear intents; zero them (and Velocity) to stop a body") {
    PhysicsApp p;
    World &w = p.world();
    const Entity walker = spawn_top_down(w, {0, 0}, 1);
    w.get<TopDownMotor>(walker).move = {1, 0};
    p.ticks(30);
    CHECK(w.get<TopDownMotor>(walker).move == Vec2{1, 0});        // still the game's value
    const float x30 = pos_of(w, walker).x;
    p.ticks(30);
    CHECK(pos_of(w, walker).x > x30 + 40.0f);                    // it kept walking
    w.get<TopDownMotor>(walker).move = {};
    w.get<Velocity>(walker).linear = {};
    const float stopped = pos_of(w, walker).x;
    p.ticks(10);
    CHECK(pos_of(w, walker).x == stopped);

    const Entity jumper = spawn_platformer(w, {300, 0});
    w.get<PlatformerMotor>(jumper).drop = true;
    w.get<PlatformerMotor>(jumper).climb_y = -1;
    w.get<PlatformerMotor>(jumper).jump_held = true;
    p.ticks(5);
    const PlatformerMotor &m = w.get<PlatformerMotor>(jumper);
    CHECK(m.drop);
    CHECK(m.climb_y == -1.0f);
    CHECK(m.jump_held);
}

TEST_CASE("drop is held, climb_y -1 is up and 0 never grabs a ladder") {
    PhysicsApp p;
    World &w = p.world();
    p.tiles() = grid_from({
        "..........",
        "..........",
        "....H.....",
        "....H.....",
        "....H.....",
        "==========",
        "..........",
        "..........",
        "##########",
    });
    // Standing on the one-way row, in the ladder column.
    const Entity hero = spawn_platformer(w, {72, 73}, {5, 7});
    p.ticks(30);
    PlatformerMotor &m = w.get<PlatformerMotor>(hero);
    REQUIRE(m.on_ground);
    CHECK_FALSE(m.on_ladder);                                    // climb_y == 0: never grabs
    m.climb_y = -1;                                              // up
    p.ticks(10);
    CHECK(w.get<PlatformerMotor>(hero).on_ladder);
    CHECK(pos_of(w, hero).y < 73.0f);
    w.get<PlatformerMotor>(hero).climb_y = 0;                    // holds on, no gravity
    const float held = pos_of(w, hero).y;
    p.ticks(10);
    CHECK(pos_of(w, hero).y == held);
    // Let go by jumping; land back on the platform; then hold drop to fall through it.
    w.get<PlatformerMotor>(hero).jump_pressed = true;
    p.ticks(1);
    w.get<PlatformerMotor>(hero).jump_pressed = false;
    CHECK_FALSE(w.get<PlatformerMotor>(hero).on_ladder);
    p.ticks(90);
    REQUIRE(w.get<PlatformerMotor>(hero).on_ground);
    CHECK(pos_of(w, hero).y == doctest::Approx(73.0f));          // back on the one-way row
    w.get<PlatformerMotor>(hero).drop = true;
    p.ticks(60);
    CHECK(pos_of(w, hero).y == doctest::Approx(121.0f));         // through it, onto the floor
}

// ---- Hazards ------------------------------------------------------------------------------------

TEST_CASE("hazard tiles report every tick while touched, spikes included") {
    PhysicsApp p;
    World &w = p.world();
    p.tiles() = grid_from({
        "..........",
        "..........",
        "...~......",
        "##^#######",
    });
    // A body standing on the solid hazard tile (column 2) and one inside the spikes (column 3).
    const Entity on_floor = spawn_kinematic(w, {40, 40}, box_collider({4, 8}));
    const Entity in_spikes = spawn_kinematic(w, {56, 40}, box_collider({4, 4}));
    const Entity crate = spawn_static(w, {40, 40}, box_collider({4, 8}));   // statics never report
    const Entity clear = spawn_kinematic(w, {120, 30}, box_collider({4, 4}));
    p.ticks(5);
    int floor_hits = 0;
    int spike_hits = 0;
    for (const auto &h : p.events().hazards) {
        CHECK(h.e.entity != crate);
        CHECK(h.e.entity != clear);
        if (h.e.entity == on_floor) {
            ++floor_hits;
            CHECK(h.e.tile == IVec2{2, 3});
        }
        if (h.e.entity == in_spikes) {
            ++spike_hits;
            CHECK(h.e.tile == IVec2{3, 2});
        }
    }
    CHECK(floor_hits == 5);
    CHECK(spike_hits == 5);
}

// ---- Projectiles --------------------------------------------------------------------------------

TEST_CASE("a projectile that hits a tile rests there; physics never destroys it") {
    PhysicsApp p;
    World &w = p.world();
    p.tiles() = grid_from({"....#....", ".........", "........."});
    const Entity e = w.create();
    w.add<Transform>(e, Transform{{8, 8}});
    w.add<Velocity>(e, Velocity{{6000, 0}});
    w.add<Projectile>(e, Projectile{.radius = 1});
    p.ticks(1);
    REQUIRE(p.events().hits.size() == 1);
    CHECK(p.events().hits[0].e.tile);
    CHECK(p.events().hits[0].e.target == Entity{});
    CHECK(p.events().hits[0].e.point.x == doctest::Approx(63.0f));
    // It stops flush with the wall, zero Velocity, still alive; the game destroys it.
    REQUIRE(w.alive(e));
    CHECK(pos_of(w, e).x == doctest::Approx(63.0f));
    CHECK(w.get<Velocity>(e).linear == Vec2{});
    CHECK_FALSE(w.has<Lifetime>(e));
    // Resting against the wall it reports nothing more, however long it stays.
    p.ticks(30);
    CHECK(p.events().hits.size() == 1);
    CHECK(pos_of(w, e).x == doctest::Approx(63.0f));
    // Given a new Velocity (a bounce written by the game) it flies again.
    w.get<Velocity>(e).linear = {-600, 0};
    p.ticks(1);
    CHECK(pos_of(w, e).x == doctest::Approx(53.0f));
    CHECK(p.events().hits.size() == 1);
}

// ---- Query freshness and restore determinism ----------------------------------------------------

namespace {

// Simulate: every seeker steers toward the nearest body overlapping its search circle.
struct Seeker {};
struct SeekLog {
    std::uint64_t found = 0;
};

void seek_system(Ctx &ctx) {
    World &w = ctx.world();
    std::array<Entity, 8> near{};
    SeekLog *log = ctx.try_local<SeekLog>();
    for (auto [e, t, v] : w.view<Transform, Velocity>(With<Seeker>{})) {
        const std::size_t n = overlap_circle(w, Circle{t.position, 40}, 2u, near);
        Vec2 steer;
        for (std::size_t i = 0; i < n && i < near.size(); ++i) {
            steer += seek(t.position, w.get<Transform>(near[i]).position, 10);
        }
        steer += separation(w, e, 12, 20, 1u);
        v.linear = steer;
        if (log != nullptr) log->found += n;
    }
}

} // namespace

TEST_CASE("queries in Simulate continue bit-identically after an undo") {
    const auto setup = [](PhysicsApp &p) {
        World &w = p.world();
        p.app().register_component<Seeker>();
        w.resources().insert<SeekLog>();
        p.app().add_system(Stage::Simulate, &seek_system, "test.seek");
        Rng rng(77);
        for (int i = 0; i < 24; ++i) {
            const Entity s = spawn_kinematic(w, {rng.range(0.0f, 200.0f), rng.range(0.0f, 200.0f)}, circle_collider(3, 1, 0));
            w.add<Seeker>(s);
        }
        for (int i = 0; i < 12; ++i) {
            spawn_kinematic(w, {rng.range(0.0f, 200.0f), rng.range(0.0f, 200.0f)}, circle_collider(4, 2, 0),
                            {rng.range(-30.0f, 30.0f), rng.range(-30.0f, 30.0f)});
        }
    };
    PhysicsApp a;
    setup(a);
    std::vector<std::uint64_t> continuous;
    for (int t = 0; t < 120; ++t) {
        a.ticks(1);
        continuous.push_back(a.world().state_hash());
    }
    CHECK(a.world().resources().get<SeekLog>().found > 0);

    PhysicsApp b;
    setup(b);
    b.ticks(60);
    WorldSnapshot snap;
    b.world().snapshot_into(snap);
    // Wander off: teleport everything, run, then undo back to tick 60.
    for (auto [e, t] : b.world().view<Transform>()) {
        (void)e;
        t.position += Vec2{500, -300};
    }
    b.ticks(7);
    b.world().restore(snap, RestoreOptions{.tick = true});
    for (int t = 60; t < 120; ++t) {
        b.ticks(1);
        REQUIRE(b.world().state_hash() == continuous[static_cast<std::size_t>(t)]);
    }
}

TEST_CASE("early-stage queries see bodies game code moved since the last Physics stage") {
    PhysicsApp p;
    World &w = p.world();
    const Entity body = spawn_kinematic(w, {0, 0}, box_collider({4, 4}));
    p.ticks(2);                                                  // grids built with it at the origin
    // Game code moves it far away after the broadphase (a teleport in PostPhysics or TickEnd).
    w.get<Transform>(body).position = {5000, 5000};
    std::array<Entity, 4> out{};
    // The first query of a tick rebuilds; within one tick refresh_queries() does it on demand.
    w.advance_tick();
    CHECK(overlap_box(w, Rect::from_center({5000, 5000}, {8, 8}), 0xFFFFFFFFu, out) == 1);
    CHECK(out[0] == body);
    CHECK(overlap_box(w, Rect::from_center({0, 0}, {8, 8}), 0xFFFFFFFFu, out) == 0);
    w.get<Transform>(body).position = {-7000, 3000};
    refresh_queries(w);
    CHECK(overlap_box(w, Rect::from_center({-7000, 3000}, {8, 8}), 0xFFFFFFFFu, out) == 1);
    CHECK(raycast(w, {-7100, 3000}, {1, 0}, 200)->entity == body);
}

// ---- Separation ---------------------------------------------------------------------------------

TEST_CASE("separation can ignore other layers; the pure form needs no World") {
    PhysicsApp p;
    World &w = p.world();
    const Entity wolf = spawn_static(w, {0, 0}, box_collider({2, 2}, 2));
    spawn_static(w, {4, 0}, box_collider({2, 2}, 2));            // pack mate on the right
    spawn_static(w, {-3, 0}, box_collider({2, 2}, 1));           // the player, closer on the left
    const Vec2 all = separation(w, wolf, 10, 30);
    const Vec2 pack = separation(w, wolf, 10, 30, 2u);
    CHECK(all.x > 0.0f);                                         // the closer player wins
    CHECK(pack.x < 0.0f);                                        // only the mate pushes
    CHECK(pack.x == doctest::Approx(-30.0f * (1.0f - 4.0f / 10.0f)));
    // In strength's units, limited to `strength`.
    CHECK(length(separation(w, wolf, 10, 5)) <= 5.0001f);

    // Pure form: same rule over caller positions; a stack splits by position in the span.
    const std::array<Vec2, 3> stack{Vec2{10, 10}, Vec2{10, 10}, Vec2{10, 10}};
    const Vec2 first = separation(0, stack, 8, 30);
    const Vec2 middle = separation(1, stack, 8, 30);
    const Vec2 last = separation(2, stack, 8, 30);
    CHECK(first.x < 0.0f);
    CHECK(last.x > 0.0f);
    CHECK(middle.x == doctest::Approx(0.0f));
    CHECK(middle.y != 0.0f);
    const std::array<Vec2, 2> pair{Vec2{0, 0}, Vec2{4, 0}};
    CHECK(separation(0, pair, 10, 30).x == doctest::Approx(-18.0f));
    CHECK(separation(5, pair, 10, 30) == Vec2{});                // out of range
    CHECK(separation(0, pair, 0, 30) == Vec2{});
}

// ---- RayQuery -----------------------------------------------------------------------------------

TEST_CASE("rays can skip tiles, see triggers and ignore the shooter") {
    PhysicsApp p;
    World &w = p.world();
    p.tiles() = grid_from({
        "..........",
        "...#......",
        "..........",
    });
    const Entity shooter = spawn_static(w, {8, 24}, box_collider({4, 4}));
    Collider zone = box_collider({4, 4});
    zone.trigger = true;
    const Entity trigger = spawn_static(w, {40, 24}, zone);
    const Entity target = spawn_static(w, {100, 24}, box_collider({4, 4}));
    // Default: the shooter is hit first unless ignored; the wall (x 48..64) stops the ray.
    CHECK(raycast(w, {8, 24}, {1, 0}, 200)->entity == shooter);
    const std::optional<RayHit> wall = raycast(w, {8, 24}, {1, 0}, 200, RayQuery{.ignore = shooter});
    REQUIRE(wall);
    CHECK(wall->tile);
    CHECK(wall->cell == IVec2{3, 1});
    CHECK(wall->point.x == doctest::Approx(48.0f));
    // Tiles off: through the wall to the target.
    const std::optional<RayHit> through = raycast(w, {8, 24}, {1, 0}, 200, RayQuery{.ignore = shooter, .tiles = false});
    REQUIRE(through);
    CHECK(through->entity == target);
    CHECK(through->distance == doctest::Approx(88.0f));
    // Triggers on: the trigger zone stops it first.
    const std::optional<RayHit> zone_hit = raycast(w, {8, 24}, {1, 0}, 200, RayQuery{.ignore = shooter, .triggers = true});
    REQUIRE(zone_hit);
    CHECK(zone_hit->entity == trigger);
    // The mask filters entity layers only; tiles still block.
    CHECK(raycast(w, {8, 24}, {1, 0}, 200, RayQuery{.mask = 2, .tiles = false}) == std::nullopt);
    const std::optional<RayHit> swept = sweep_circle(w, {8, 24}, {200, 24}, 2, RayQuery{.ignore = shooter, .tiles = false});
    REQUIRE(swept);
    CHECK(swept->entity == target);
    CHECK(swept->point.x == doctest::Approx(94.0f));
}

// ---- Box sweeps leaving the map -----------------------------------------------------------------

TEST_CASE("a box sweep that leaves the map is blocked when outside is solid") {
    TileWorld g = grid_from({
        "......",
        "......",
        "......",
    });
    const Rect box = lam::test::feet_box(0, 0, 8, 8);            // relative to the feet
    CHECK(box_path_clear(g, box, {12, 40}, {84, 40}));
    CHECK(box_path_clear(g, box, {4, 48}, {92, 8}));             // touching the edges from inside
    CHECK_FALSE(box_path_clear(g, box, {12, 40}, {200, 40}));    // out through the right side
    CHECK_FALSE(box_path_clear(g, box, {500, 500}, {510, 500})); // entirely outside
    CHECK_FALSE(box_path_clear(g, box, {3, 40}, {12, 40}));      // starts half outside
    g.outside_solid = false;
    CHECK(box_path_clear(g, box, {500, 500}, {510, 500}));
    CHECK(box_path_clear(g, box, {12, 40}, {200, 40}));
}

// ---- TileWorld on a bare World ------------------------------------------------------------------

TEST_CASE("ensure_tile_world on a bare World; an empty TileWorld has no tiles") {
    World w;
    CHECK(w.resources().try_get<TileWorld>() == nullptr);
    TileWorld &tw = ensure_tile_world(w);
    CHECK(&ensure_tile_world(w) == &tw);                         // inserted once
    CHECK(tw.shapes.width() == 0);
    // Empty (0 x 0): open space for the physics functions, whatever outside_solid says.
    CHECK(tw.outside_solid);
    const BoxMove m = move_box(tw, Rect{0, 0, 8, 8}, {100, 50});
    CHECK(m.box.min() == Vec2{100, 50});
    CHECK_FALSE(m.hit_x);
    CHECK_FALSE(box_blocked(tw, Rect{0, 0, 8, 8}));
    CHECK(tile_ray_clear(tw, {0, 0}, {300, 300}));
    CHECK(box_path_clear(tw, Rect{0, 0, 8, 8}, {0, 0}, {300, 300}));
    // move_body works on a bare World too (tiles only: no PhysicsWorld, no entity solids).
    const Entity e = w.create();
    w.add<Transform>(e, Transform{{12, 8}});
    w.add<Collider>(e, box_collider({4, 4}));
    tw = grid_from({"....#....", ".........", "........."});
    const BoxMove moved = move_body(w, e, {200, 0}, 1);
    CHECK(moved.hit_x);
    CHECK(w.get<Transform>(e).position.x == doctest::Approx(60.0f));
}

// ---- Steady state ------------------------------------------------------------------------------

TEST_CASE("queries: the filtered query overloads do not allocate in steady state") {
    PhysicsApp p;
    World &w = p.world();
    p.events().enabled = false;
    p.tiles() = grid_from({"..........", "...#......", ".........."});
    std::vector<Entity> bodies;
    for (int i = 0; i < 16; ++i) bodies.push_back(spawn_dynamic(w, {10.0f * static_cast<float>(i), 20}, circle_collider(3)));
    std::array<Vec2, 16> positions{};
    lam::test::run_ticks(p.app(), 120);
    float sum = 0;
    CHECK_NO_ALLOC({
        for (int t = 0; t < 200; ++t) {
            for (std::size_t i = 0; i < positions.size(); ++i) positions[i] = pos_of(w, bodies[i]);
            sum += separation(w, bodies[0], 20, 10, 1u).x;
            sum += separation(3, positions, 20, 10).x;
            if (auto h = raycast(w, {0, 24}, {1, 0}, 200, RayQuery{.tiles = false, .triggers = true})) sum += h->distance;
            if (auto h = sweep_circle(w, {0, 10}, {200, 10}, 2, RayQuery{})) sum += h->distance;
            refresh_queries(w);
            lam::test::run_ticks(p.app(), 1);
        }
    });
    CHECK(sum != 0.0f);
}
