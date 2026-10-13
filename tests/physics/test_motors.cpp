// PlatformerMotor, TopDownMotor, PathFollower/MovingPlatform and move_body, run through
// PhysicsPlugin.
#include <cmath>
#include <limits>
#include <vector>

#include <lam/test/support.h>

#include "physics_app.h"
#include "physics_test_util.h"

using namespace lam;
using lam::test::box_collider;
using lam::test::grid_from;
using lam::test::PhysicsApp;
using lam::test::pos_of;
using lam::test::spawn_kinematic;
using lam::test::spawn_static;

namespace {

constexpr float kDt = 1.0f / 60.0f;

Entity spawn_platformer(World &w, Vec2 pos, Vec2 half = {5, 7}) {
    const Entity e = w.create();
    w.add<Transform>(e, Transform{pos});
    w.add<Collider>(e, box_collider(half));
    w.add<PlatformerMotor>(e);
    return e;
}

Entity spawn_top_down(World &w, Vec2 pos, Vec2 half = {5, 3}) {
    const Entity e = w.create();
    w.add<Transform>(e, Transform{pos});
    w.add<Collider>(e, box_collider(half));
    w.add<TopDownMotor>(e);
    return e;
}

float bottom(World &w, Entity e) {
    const Collider &c = w.get<Collider>(e);
    return pos_of(w, e).y + c.offset.y + c.half.y;
}

// 20 x 12, floor on rows 10-11 (y = 160), walls in columns 0 and 19.
void floor_room(TileWorld &tw) {
    tw = grid_from({
        "#..................#",
        "#..................#",
        "#..................#",
        "#..................#",
        "#..................#",
        "#..................#",
        "#..................#",
        "#..................#",
        "#..................#",
        "#..................#",
        "####################",
        "####################",
    });
}

// Ticks until the motor stands on the ground (at most `limit`); returns the ticks used.
int settle(PhysicsApp &p, Entity e, int limit = 240) {
    for (int i = 0; i < limit; ++i) {
        if (p.world().get<PlatformerMotor>(e).on_ground) return i;
        p.ticks(1);
    }
    return -1;
}

} // namespace

TEST_CASE("platformer: falls with its own gravity, capped at max_fall, and lands flush") {
    PhysicsApp p;
    floor_room(p.tiles());
    World &w = p.world();
    const Entity hero = spawn_platformer(w, {80, 20});
    float fastest = 0.0f;
    for (int i = 0; i < 60; ++i) {
        p.ticks(1);
        const float vy = w.get<Velocity>(hero).linear.y;
        if (vy > fastest) fastest = vy;
        CHECK(vy <= w.get<PlatformerMotor>(hero).max_fall);
    }
    CHECK(fastest > 0.0f);
    const PlatformerMotor &m = w.get<PlatformerMotor>(hero);
    CHECK(m.on_ground);
    CHECK(bottom(w, hero) <= 160.0f);
    CHECK(bottom(w, hero) > 159.99f);
    CHECK(w.get<Velocity>(hero).linear.y == 0.0f);
    CHECK(m.ground_normal == Vec2{0, -1});
    CHECK_FALSE(m.on_slope);
}

TEST_CASE("platformer: runs up to run_speed by accel, stops by decel; less control in the air") {
    PhysicsApp p;
    floor_room(p.tiles());
    World &w = p.world();
    const Entity hero = spawn_platformer(w, {80, 152});
    REQUIRE(settle(p, hero) >= 0);
    PlatformerMotor &m = w.get<PlatformerMotor>(hero);
    m.move_x = 1.0f;
    int ticks = 0;
    while (w.get<Velocity>(hero).linear.x < m.run_speed && ticks < 100) {
        p.ticks(1);
        ++ticks;
    }
    CHECK(ticks == 8);                                           // 110 / (900 / 60) = 7.33
    CHECK(w.get<Velocity>(hero).linear.x == m.run_speed);
    w.get<PlatformerMotor>(hero).move_x = 0.0f;
    ticks = 0;
    while (w.get<Velocity>(hero).linear.x > 0.0f && ticks < 100) {
        p.ticks(1);
        ++ticks;
    }
    CHECK(ticks == 6);                                           // 110 / (1200 / 60) = 5.5
    // In the air the same input changes speed by air_control as much.
    w.get<PlatformerMotor>(hero).jump_pressed = true;
    w.get<PlatformerMotor>(hero).jump_held = true;
    p.ticks(1);
    w.get<PlatformerMotor>(hero).jump_pressed = false;
    w.get<PlatformerMotor>(hero).move_x = 1.0f;
    p.ticks(1);
    CHECK(w.get<Velocity>(hero).linear.x == doctest::Approx(900.0f * 0.7f * kDt));
}

TEST_CASE("platformer: a held jump rises about v^2 / 2g; a tap (jump cut) rises less") {
    const auto apex = [](bool held) {
        PhysicsApp p;
        floor_room(p.tiles());
        World &w = p.world();
        const Entity hero = spawn_platformer(w, {80, 152});
        REQUIRE(settle(p, hero) >= 0);
        const float ground = bottom(w, hero);
        w.get<PlatformerMotor>(hero).jump_pressed = true;
        w.get<PlatformerMotor>(hero).jump_held = true;
        p.ticks(1);
        CHECK_FALSE(w.get<PlatformerMotor>(hero).on_ground);
        w.get<PlatformerMotor>(hero).jump_pressed = false;
        w.get<PlatformerMotor>(hero).jump_held = held;
        float top = ground;
        for (int i = 0; i < 120; ++i) {
            p.ticks(1);
            top = std::min(top, bottom(w, hero));
        }
        CHECK(w.get<PlatformerMotor>(hero).on_ground);
        return ground - top;
    };
    const float full = apex(true);
    const float tap = apex(false);
    CHECK(full == doctest::Approx(280.0f * 280.0f / (2.0f * 900.0f)).epsilon(0.08));
    CHECK(tap < full * 0.5f);
    CHECK(tap > 0.0f);
}

TEST_CASE("platformer: coyote time accepts a jump for coyote_ticks after leaving a ledge") {
    // A ledge ending at x = 96; the hero walks off it to the right.
    const auto jumps_after = [](int late) {
        PhysicsApp p;
        p.tiles() = grid_from({
            "..............",
            "..............",
            "..............",
            "..............",
            "######........",
            "######........",
            "######........",
            "##############",
        });
        World &w = p.world();
        const Entity hero = spawn_platformer(w, {80, 50});
        REQUIRE(settle(p, hero) >= 0);
        w.get<PlatformerMotor>(hero).move_x = 1.0f;
        int guard = 0;
        while (w.get<PlatformerMotor>(hero).on_ground && guard++ < 200) p.ticks(1);
        p.ticks(late);
        w.get<PlatformerMotor>(hero).jump_pressed = true;
        w.get<PlatformerMotor>(hero).jump_held = true;
        p.ticks(1);
        return w.get<Velocity>(hero).linear.y < -200.0f;
    };
    // coyote_ticks = 6: the jump is still accepted on the 6 ticks after the last grounded one.
    for (int late = 0; late <= 5; ++late) {
        CAPTURE(late);
        CHECK(jumps_after(late));
    }
    CHECK_FALSE(jumps_after(6));
    CHECK_FALSE(jumps_after(10));
}

TEST_CASE("platformer: a jump pressed shortly before landing fires on landing (buffer)") {
    const auto buffered = [](int early) {
        PhysicsApp p;
        floor_room(p.tiles());
        World &w = p.world();
        const Entity hero = spawn_platformer(w, {80, 40});
        // Find the landing tick first with a dry run of the same fall.
        int land = 0;
        {
            PhysicsApp q;
            floor_room(q.tiles());
            const Entity probe = spawn_platformer(q.world(), {80, 40});
            land = settle(q, probe);
        }
        REQUIRE(land > early);
        p.ticks(land - early);
        w.get<PlatformerMotor>(hero).jump_pressed = true;
        w.get<PlatformerMotor>(hero).jump_held = true;
        p.ticks(1);
        w.get<PlatformerMotor>(hero).jump_pressed = false;
        bool jumped = false;
        for (int i = 0; i < early + 3; ++i) {
            p.ticks(1);
            if (w.get<Velocity>(hero).linear.y < -200.0f) jumped = true;
        }
        return jumped;
    };
    CHECK(buffered(1));
    CHECK(buffered(4));
    CHECK_FALSE(buffered(12));
}

TEST_CASE("platformer: walls and ceilings stop it and are reported") {
    PhysicsApp p;
    p.tiles() = grid_from({
        "##########",
        "#........#",
        "#........#",
        "#...######",
        "#........#",
        "##########",
    });
    World &w = p.world();
    const Entity hero = spawn_platformer(w, {40, 70});
    REQUIRE(settle(p, hero) >= 0);
    PlatformerMotor &m = w.get<PlatformerMotor>(hero);
    m.move_x = -1.0f;
    p.ticks(60);
    CHECK(w.get<PlatformerMotor>(hero).on_wall_left);
    CHECK(pos_of(w, hero).x - 5.0f == doctest::Approx(16.0f).epsilon(1e-4));
    CHECK(w.get<Velocity>(hero).linear.x == 0.0f);
    w.get<PlatformerMotor>(hero).move_x = 0.0f;
    // Under the ledge (row 3, y 48..64): a jump bumps the ceiling.
    w.get<Transform>(hero).position.x = 80.0f;
    w.get<PlatformerMotor>(hero).jump_pressed = true;
    w.get<PlatformerMotor>(hero).jump_held = true;
    bool ceiling = false;
    for (int i = 0; i < 20; ++i) {
        p.ticks(1);
        w.get<PlatformerMotor>(hero).jump_pressed = false;
        if (w.get<PlatformerMotor>(hero).on_ceiling) {
            ceiling = true;
            CHECK(pos_of(w, hero).y - 7.0f >= 64.0f);
            CHECK(w.get<Velocity>(hero).linear.y >= 0.0f);
        }
    }
    CHECK(ceiling);
}

TEST_CASE("platformer: walks up a slope and back down it without leaving the ground") {
    PhysicsApp p;
    p.tiles() = grid_from({
        "............",
        "............",
        "......./####",
        "....../#####",
        "...../######",
        "############",
    });
    World &w = p.world();
    const Entity hero = spawn_platformer(w, {24, 70}, {4, 6});
    REQUIRE(settle(p, hero) >= 0);
    PlatformerMotor &m0 = w.get<PlatformerMotor>(hero);
    m0.move_x = 1.0f;
    bool saw_slope = false;
    for (int i = 0; i < 90; ++i) {
        p.ticks(1);
        const PlatformerMotor &m = w.get<PlatformerMotor>(hero);
        if (m.on_slope) {
            saw_slope = true;
            CHECK(m.ground_normal.x == doctest::Approx(-0.7071f).epsilon(1e-3));
            CHECK(m.ground_normal.y == doctest::Approx(-0.7071f).epsilon(1e-3));
        }
    }
    CHECK(saw_slope);
    CHECK(bottom(w, hero) == doctest::Approx(32.0f));            // on the plateau (row 2 top)
    CHECK(w.get<PlatformerMotor>(hero).on_ground);
    // Back down: ground snap keeps it on the slope every tick.
    w.get<PlatformerMotor>(hero).move_x = -1.0f;
    for (int i = 0; i < 90; ++i) {
        p.ticks(1);
        CHECK(w.get<PlatformerMotor>(hero).on_ground);
    }
    CHECK(bottom(w, hero) == doctest::Approx(80.0f));
}

TEST_CASE("platformer: one-way platforms hold from above, pass from below, and drop through") {
    PhysicsApp p;
    p.tiles() = grid_from({
        "..........",
        "..........",
        "..........",
        "..........",
        "...====...",
        "..........",
        "##########",
    });
    World &w = p.world();
    const Entity hero = spawn_platformer(w, {72, 85});
    REQUIRE(settle(p, hero) >= 0);
    CHECK(bottom(w, hero) == doctest::Approx(96.0f));            // floor
    // Jump up through the platform (row 4, top y = 64) and land on it.
    w.get<PlatformerMotor>(hero).jump_pressed = true;
    w.get<PlatformerMotor>(hero).jump_held = true;
    p.ticks(1);
    w.get<PlatformerMotor>(hero).jump_pressed = false;
    p.ticks(90);
    CHECK(w.get<PlatformerMotor>(hero).on_ground);
    CHECK(bottom(w, hero) == doctest::Approx(64.0f));
    // Drop through it.
    w.get<PlatformerMotor>(hero).drop = true;
    p.ticks(2);
    w.get<PlatformerMotor>(hero).drop = false;
    p.ticks(60);
    CHECK(bottom(w, hero) == doctest::Approx(96.0f));
}

TEST_CASE("platformer: entity one-way platforms and solids, and solid_mask") {
    PhysicsApp p;
    floor_room(p.tiles());
    World &w = p.world();
    Collider plank = box_collider({24, 2});
    plank.one_way = true;
    spawn_static(w, {80, 120}, plank);                          // top at 118
    const Entity crate = spawn_static(w, {150, 152}, box_collider({8, 8}));   // top at 144
    Collider ghost_c = box_collider({8, 8}, 2);                  // layer 2: not in solid_mask 1
    spawn_static(w, {40, 152}, ghost_c);
    const Entity hero = spawn_platformer(w, {80, 60});
    REQUIRE(settle(p, hero) >= 0);
    CHECK(bottom(w, hero) == doctest::Approx(118.0f));
    // Drop through the entity plank onto the floor.
    w.get<PlatformerMotor>(hero).drop = true;
    p.ticks(2);
    w.get<PlatformerMotor>(hero).drop = false;
    p.ticks(60);
    CHECK(bottom(w, hero) == doctest::Approx(160.0f));
    // Walk right into the crate: blocked flush.
    w.get<PlatformerMotor>(hero).move_x = 1.0f;
    p.ticks(90);
    CHECK(w.get<PlatformerMotor>(hero).on_wall_right);
    CHECK(pos_of(w, hero).x + 5.0f == doctest::Approx(142.0f).epsilon(1e-4));
    // Walk left through the layer-2 box: nothing stops it until the room wall.
    w.get<PlatformerMotor>(hero).move_x = -1.0f;
    p.ticks(120);
    CHECK(pos_of(w, hero).x - 5.0f == doctest::Approx(16.0f).epsilon(1e-4));
    (void)crate;
}

TEST_CASE("platformer: ladders are climbed without gravity and left by jumping") {
    PhysicsApp p;
    p.tiles() = grid_from({
        "..........",
        "....H.....",
        "....H.....",
        "....H.....",
        "....H.....",
        "##########",
    });
    World &w = p.world();
    const Entity hero = spawn_platformer(w, {72, 70}, {4, 6});
    REQUIRE(settle(p, hero) >= 0);
    PlatformerMotor &m = w.get<PlatformerMotor>(hero);
    m.climb_y = -1.0f;
    p.ticks(1);
    CHECK(w.get<PlatformerMotor>(hero).on_ladder);
    const float start = pos_of(w, hero).y;
    p.ticks(30);
    // 60 px/s for 30 ticks.
    CHECK(pos_of(w, hero).y == doctest::Approx(start - 30.0f).epsilon(1e-3));
    // Hanging on: no input, no fall.
    w.get<PlatformerMotor>(hero).climb_y = 0.0f;
    const float hang = pos_of(w, hero).y;
    p.ticks(30);
    CHECK(pos_of(w, hero).y == hang);
    CHECK(w.get<PlatformerMotor>(hero).on_ladder);
    // Jump off.
    w.get<PlatformerMotor>(hero).jump_pressed = true;
    p.ticks(1);
    CHECK_FALSE(w.get<PlatformerMotor>(hero).on_ladder);
    CHECK(w.get<Velocity>(hero).linear.y < 0.0f);
    w.get<PlatformerMotor>(hero).jump_pressed = false;
    p.ticks(120);
    CHECK(w.get<PlatformerMotor>(hero).on_ground);
}

TEST_CASE("platformer: a moving platform carries its rider and reports riding") {
    PhysicsApp p;
    floor_room(p.tiles());
    World &w = p.world();
    const Entity platform = spawn_static(w, {60, 100}, box_collider({16, 4}));   // top at 96
    w.add<MovingPlatform>(platform);
    PathFollower path;
    path.points[0] = {60, 100};
    path.points[1] = {160, 100};
    path.count = 2;
    path.speed = 30.0f;
    w.add<PathFollower>(platform, path);
    w.add<Velocity>(platform);
    const Entity hero = spawn_platformer(w, {60, 80});
    REQUIRE(settle(p, hero) >= 0);
    CHECK(w.get<PlatformerMotor>(hero).riding == platform);
    const float offset = pos_of(w, hero).x - pos_of(w, platform).x;
    const float start = pos_of(w, platform).x;
    p.ticks(120);
    CHECK(w.get<PlatformerMotor>(hero).riding == platform);
    CHECK(pos_of(w, platform).x == doctest::Approx(start + 60.0f).epsilon(1e-3));
    CHECK(pos_of(w, hero).x - pos_of(w, platform).x == doctest::Approx(offset).epsilon(1e-3));
    CHECK(bottom(w, hero) == doctest::Approx(96.0f).epsilon(1e-4));
    CHECK(w.get<MovingPlatform>(platform).last_delta.x == doctest::Approx(0.5f));
    CHECK(w.get<Velocity>(platform).linear.x == doctest::Approx(30.0f));
}

TEST_CASE("platformer: non-finite intent is ignored") {
    PhysicsApp p;
    floor_room(p.tiles());
    World &w = p.world();
    const Entity hero = spawn_platformer(w, {80, 152});
    REQUIRE(settle(p, hero) >= 0);
    w.get<PlatformerMotor>(hero).move_x = std::nanf("");
    w.get<PlatformerMotor>(hero).climb_y = std::numeric_limits<float>::infinity();
    p.ticks(10);
    CHECK(is_finite(pos_of(w, hero)));
    CHECK(w.get<Velocity>(hero).linear.x == 0.0f);
}

// ---- Top-down ----------------------------------------------------------------------------------

TEST_CASE("top-down: walks, stops at walls and remembers which way it leaned") {
    PhysicsApp p;
    TileWorld &tw = p.tiles();
    tw.resize(10, 3, 16, {});
    for (int x = 0; x < 10; ++x) tw.set({x, 0}, TileShape::Solid, 0);
    World &w = p.world();
    // A 10 x 6 foot box at feet (40, 40): a Box collider of half (5, 3) at (40, 37).
    const Entity walker = spawn_top_down(w, {40, 37});
    w.get<TopDownMotor>(walker).move = {0, -1};
    p.ticks(120);
    CHECK(pos_of(w, walker).y == doctest::Approx(19.0f));        // box top flush with the wall at 16
    CHECK(w.get<TopDownMotor>(walker).wall.y == -1);
    CHECK(w.get<Velocity>(walker).linear == Vec2{0, 0});         // pressed on the wall it does not move
}

TEST_CASE("top-down: accelerates to max_speed in 6 ticks and stops in 5 (walker preset)") {
    PhysicsApp p;
    World &w = p.world();
    const Entity e = spawn_top_down(w, {0, 0});
    w.get<TopDownMotor>(e).move = {1, 0};
    int ticks = 0;
    while (w.get<Velocity>(e).linear.x != 90.0f && ticks < 100) {
        p.ticks(1);
        ++ticks;
    }
    CHECK(ticks == 6);
    w.get<TopDownMotor>(e).move = {};
    w.get<Velocity>(e).linear = {-63.0f, 51.0f};
    ticks = 0;
    while (w.get<Velocity>(e).linear != Vec2{} && ticks < 100) {
        p.ticks(1);
        ++ticks;
    }
    CHECK(ticks == 5);
    // A stick pushed past 1 is clamped: no faster diagonals.
    w.get<TopDownMotor>(e).move = {5, 5};
    p.ticks(30);
    CHECK(length(w.get<Velocity>(e).linear) == doctest::Approx(90.0f));
}

TEST_CASE("top-down: corner assist steers round a corner it only clips") {
    PhysicsApp p;
    p.tiles() = grid_from({
        "..........",
        "....#.....",
        "..........",
    });
    World &w = p.world();
    // The pillar is x 64..80, y 16..32. Walking right with the box at y 12..18 clips it by 2 px.
    const Entity e = spawn_top_down(w, {40, 15}, {5, 3});
    w.get<TopDownMotor>(e).move = {1, 0};
    p.ticks(60);
    CHECK(pos_of(w, e).x > 90.0f);                               // went past the pillar (x 64..80)
    CHECK(pos_of(w, e).y + 3.0f <= 16.0f);                       // slid up round it
    // With no assist it stops flush.
    const Entity stuck = spawn_top_down(w, {40, 31});            // box y 28..34: clips it by 4 px
    w.get<TopDownMotor>(stuck).corner_assist = 0.0f;
    w.get<TopDownMotor>(stuck).move = {1, 0};
    p.ticks(60);
    CHECK(pos_of(w, stuck).x + 5.0f == doctest::Approx(64.0f));
    CHECK(w.get<TopDownMotor>(stuck).wall.x == 1);
}

TEST_CASE("top-down: motors are solid to each other by layer; triggers never block") {
    PhysicsApp p;
    World &w = p.world();
    const Entity a = spawn_top_down(w, {0, 0});
    const Entity b = spawn_top_down(w, {40, 0});
    Collider zone = box_collider({4, 30});
    zone.trigger = true;
    spawn_static(w, {20, 0}, zone);
    w.get<TopDownMotor>(a).move = {1, 0};
    p.ticks(60);
    CHECK(pos_of(w, a).x + 5.0f == doctest::Approx(35.0f).epsilon(1e-4));   // flush with b
    CHECK(w.get<TopDownMotor>(a).wall.x == 1);
    CHECK(p.events().entered.size() == 1);
    // b leaves the solid layer: a walks through it.
    w.get<Collider>(b).layer = 2;
    p.ticks(60);
    CHECK(pos_of(w, a).x > 60.0f);
}

TEST_CASE("top-down: a crowd that blocks nothing skips its solid query; layer edits count") {
    // Twelve walkers on layer 2 with mask 0 (no contacts, nothing solid to them: solid_mask 1),
    // packed into one corner, walking right into a gap that a layer-2 post fills.
    PhysicsApp p;
    World &w = p.world();
    std::vector<Entity> crowd;
    for (int i = 0; i < 12; ++i) {
        const Vec2 at{static_cast<float>(i % 4) * 3.0f, static_cast<float>(i / 4) * 3.0f};
        const Entity e = spawn_top_down(w, at);
        Collider &c = w.get<Collider>(e);
        c.layer = 2;
        c.mask = 0;
        w.get<TopDownMotor>(e).move = {1, 0};
        crowd.push_back(e);
    }
    Collider post = box_collider({2, 40});
    post.layer = 2;                                             // not in any solid_mask yet
    const Entity wall = spawn_static(w, {60, 0}, post);
    p.ticks(60);
    for (const Entity e : crowd) CHECK(pos_of(w, e).x > 60.0f); // walked through the post
    CHECK(p.events().started.empty());                          // mask 0: no pairs, no contacts
    // In-place edits, no structural change: the post becomes solid and the walkers report contacts.
    w.get<Collider>(wall).layer = 1;
    for (const Entity e : crowd) {
        w.get<Transform>(e).position.x -= 80.0f;
        w.get<Collider>(e).mask = 2;
    }
    p.ticks(60);
    for (const Entity e : crowd) {
        CHECK(pos_of(w, e).x + 5.0f <= 58.0f + 1e-3f);          // stopped at the post's left face
        CHECK(w.get<TopDownMotor>(e).wall.x == 1);
    }
    CHECK_FALSE(p.events().started.empty());                    // mask 2 meets layer 2
}

// ---- Paths and move_body ---------------------------------------------------------------------------

TEST_CASE("paths: ping-pong goes to the end and back; a loop wraps to the first point") {
    PhysicsApp p;
    World &w = p.world();
    PathFollower pp;
    pp.points[0] = {0, 0};
    pp.points[1] = {30, 0};
    pp.points[2] = {30, 30};
    pp.count = 3;
    pp.speed = 60.0f;                                           // 1 px per tick
    const Entity ping = w.create();
    w.add<Transform>(ping, Transform{{0, 0}});
    w.add<PathFollower>(ping, pp);
    PathFollower loop = pp;
    loop.ping_pong = false;
    const Entity looper = w.create();
    w.add<Transform>(looper, Transform{{0, 0}});
    w.add<PathFollower>(looper, loop);
    const auto near = [](Vec2 a, Vec2 b) { return std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f; };
    p.ticks(60);                                                 // both at the end (30, 30)
    CHECK(near(pos_of(w, ping), {30, 30}));
    CHECK(near(pos_of(w, looper), {30, 30}));
    p.ticks(30);
    CHECK(near(pos_of(w, ping), {30, 0}));                       // back along the path
    CHECK_FALSE(w.get<PathFollower>(ping).forward);
    // The loop heads straight back to point 0: 30 px of the 42.4 px diagonal.
    CHECK(pos_of(w, looper).x == doctest::Approx(30.0f - 30.0f * 0.70710678f).epsilon(1e-3));
    p.ticks(30);
    CHECK(near(pos_of(w, ping), {0, 0}));
    CHECK(w.get<PathFollower>(ping).forward);
    // Degenerate paths do nothing.
    const Entity lonely = w.create();
    w.add<Transform>(lonely, Transform{{5, 5}});
    PathFollower one;
    one.points[0] = {50, 50};
    one.count = 1;
    w.add<PathFollower>(lonely, one);
    p.ticks(10);
    CHECK(pos_of(w, lonely) == Vec2{5, 5});
}

TEST_CASE("move_body: moves a custom controller against tiles and entity solids") {
    PhysicsApp p;
    floor_room(p.tiles());
    World &w = p.world();
    spawn_static(w, {100, 100}, box_collider({8, 8}));
    spawn_static(w, {100, 60}, box_collider({8, 8}, 4));         // layer 4
    const Entity self = spawn_static(w, {60, 100}, box_collider({4, 4}));
    p.ticks(1);
    BoxMove r = move_body(w, self, {100, 0}, 1);
    CHECK(r.hit_x);
    CHECK(pos_of(w, self).x + 4.0f == doctest::Approx(92.0f).epsilon(1e-4));
    // A layer outside the mask does not block; the room wall does.
    w.get<Transform>(self).position = {60, 60};
    r = move_body(w, self, {400, 0}, 1);
    CHECK(r.hit_x);
    CHECK(pos_of(w, self).x + 4.0f == doctest::Approx(304.0f).epsilon(1e-4));
    // Without a Collider nothing happens.
    const Entity bare = w.create();
    w.add<Transform>(bare, Transform{{1, 1}});
    r = move_body(w, bare, {5, 5}, 1);
    CHECK(pos_of(w, bare) == Vec2{1, 1});
}

TEST_CASE("motors: steady-state ticks do not allocate") {
    PhysicsApp p;
    floor_room(p.tiles());
    World &w = p.world();
    p.events().enabled = false;
    for (int i = 0; i < 6; ++i) {
        const Entity hero = spawn_platformer(w, {40.0f + 30.0f * static_cast<float>(i), 100});
        w.get<PlatformerMotor>(hero).move_x = i % 2 == 0 ? 1.0f : -1.0f;
        const Entity td = spawn_top_down(w, {40.0f + 30.0f * static_cast<float>(i), 40});
        w.get<TopDownMotor>(td).move = {i % 2 == 0 ? 1.0f : -1.0f, 0.3f};
    }
    const Entity platform = spawn_static(w, {150, 130}, box_collider({16, 4}));
    w.add<MovingPlatform>(platform);
    PathFollower path;
    path.points[0] = {150, 130};
    path.points[1] = {250, 130};
    path.count = 2;
    w.add<PathFollower>(platform, path);
    lam::test::run_ticks(p.app(), 120);
    CHECK_NO_ALLOC(lam::test::run_ticks(p.app(), 600));
}
