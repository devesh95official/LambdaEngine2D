// Spatial queries (raycast, sweep_circle, overlap_box/circle, query_neighbors, find_interactable)
// and swept projectiles (pierce, owner, masks, tiles, no tunnelling, ProjectileHit).
#include <array>
#include <cmath>

#include <lam/ecs/commands.h>
#include <lam/ecs/lifetime.h>
#include <lam/test/support.h>

#include "physics_app.h"
#include "physics_test_util.h"

using namespace lam;
using namespace lam::literals;
using lam::test::box_collider;
using lam::test::circle_collider;
using lam::test::grid_from;
using lam::test::PhysicsApp;
using lam::test::pos_of;
using lam::test::spawn_dynamic;
using lam::test::spawn_kinematic;
using lam::test::spawn_static;

namespace {

Entity spawn_bullet(World &w, Vec2 pos, Vec2 vel, Projectile p = {}) {
    const Entity e = w.create();
    w.add<Transform>(e, Transform{pos});
    w.add<Velocity>(e, Velocity{vel});
    w.add<Projectile>(e, p);
    return e;
}

} // namespace

TEST_CASE("queries: raycast reports the nearest body with point, normal and distance") {
    PhysicsApp p;
    World &w = p.world();
    const Entity near = spawn_static(w, {50, 0}, box_collider({5, 5}));
    const Entity far = spawn_static(w, {100, 0}, box_collider({5, 5}));
    const Entity round = spawn_static(w, {0, 60}, circle_collider(10));
    p.ticks(1);
    std::optional<RayHit> h = raycast(w, {0, 0}, {1, 0}, 200);
    REQUIRE(h.has_value());
    CHECK(h->entity == near);
    CHECK_FALSE(h->tile);
    CHECK(h->point.x == doctest::Approx(45.0f));
    CHECK(h->normal == Vec2{-1, 0});
    CHECK(h->distance == doctest::Approx(45.0f));
    // Ignore and masks.
    h = raycast(w, {0, 0}, {1, 0}, 200, 0xFFFFFFFFu, near);
    REQUIRE(h.has_value());
    CHECK(h->entity == far);
    w.get<Collider>(near).layer = 2;
    h = raycast(w, {0, 0}, {1, 0}, 200, 1);
    REQUIRE(h.has_value());
    CHECK(h->entity == far);
    // Too short, and the other way: nothing.
    CHECK_FALSE(raycast(w, {0, 0}, {1, 0}, 40).has_value());
    CHECK_FALSE(raycast(w, {0, 0}, {-1, 0}, 200).has_value());
    // A circle: normal along the radius.
    h = raycast(w, {0, 0}, {0, 1}, 200);
    REQUIRE(h.has_value());
    CHECK(h->entity == round);
    CHECK(h->point.y == doctest::Approx(50.0f));
    CHECK(h->normal.y == doctest::Approx(-1.0f));
    // A non-unit or zero direction.
    CHECK(raycast(w, {0, 0}, {0, 7}, 200).has_value());
    CHECK_FALSE(raycast(w, {0, 0}, {0, 0}, 200).has_value());
    // Starting inside a body: distance 0, normal against the ray.
    h = raycast(w, {100, 0}, {1, 0}, 50);
    REQUIRE(h.has_value());
    CHECK(h->entity == far);
    CHECK(h->distance == 0.0f);
    CHECK(h->normal == Vec2{-1, 0});
}

TEST_CASE("queries: raycast stops at tiles (with the cell) and triggers never stop it") {
    PhysicsApp p;
    p.tiles() = grid_from({
        "..........",
        "......#...",
        "..........",
    });
    World &w = p.world();
    Collider zone = box_collider({8, 8});
    zone.trigger = true;
    spawn_static(w, {40, 24}, zone);
    const Entity behind = spawn_static(w, {130, 24}, box_collider({4, 4}));
    p.ticks(1);
    std::optional<RayHit> h = raycast(w, {4, 24}, {1, 0}, 150);
    REQUIRE(h.has_value());
    CHECK(h->tile);
    CHECK(h->entity == Entity{});
    CHECK(h->cell == IVec2{6, 1});
    CHECK(h->point.x == doctest::Approx(96.0f));
    CHECK(h->normal == Vec2{-1, 0});
    // Row 0 is open up to the body.
    h = raycast(w, {4, 8}, {1, 0}, 150);
    CHECK_FALSE((h.has_value() && h->entity == behind));
    w.get<Transform>(behind).position = {130, 8};
    p.ticks(1);
    h = raycast(w, {4, 8}, {1, 0}, 150);
    REQUIRE(h.has_value());
    CHECK(h->entity == behind);
}

TEST_CASE("queries: sweep_circle is a ray with a radius") {
    PhysicsApp p;
    World &w = p.world();
    const Entity post = spawn_static(w, {50, 0}, box_collider({2, 2}));
    p.ticks(1);
    // Passing 4 px beside the post: a thin circle misses, a fat one touches its corner side.
    CHECK_FALSE(sweep_circle(w, {0, 6}, {100, 6}, 1).has_value());
    const std::optional<RayHit> h = sweep_circle(w, {0, 6}, {100, 6}, 5);
    REQUIRE(h.has_value());
    CHECK(h->entity == post);
    CHECK(h->distance > 40.0f);
    CHECK(h->distance < 50.0f);
    CHECK_FALSE(sweep_circle(w, {0, 6}, {100, 6}, 5, 0xFFFFFFFFu, post).has_value());
}

TEST_CASE("queries: overlaps are sorted by entity index and count past the output size") {
    PhysicsApp p;
    World &w = p.world();
    std::vector<Entity> made;
    for (int i = 0; i < 6; ++i) made.push_back(spawn_static(w, {static_cast<float>(i) * 10.0f, 0}, box_collider({4, 4})));
    Collider zone = circle_collider(3);
    zone.trigger = true;
    const Entity trig = spawn_static(w, {25, 0}, zone);
    spawn_static(w, {25, 100}, box_collider({4, 4}));           // far away
    const Entity other_layer = spawn_static(w, {30, 2}, box_collider({4, 4}, 8));
    p.ticks(1);
    std::array<Entity, 3> out{};
    // x 12..38 covers boxes 1..3, the edge of box 4 (x 36..44) and the trigger.
    std::size_t n = overlap_box(w, Rect{12, -1, 26, 2}, 1, out);
    CHECK(n == 5);                                                 // boxes 1, 2, 3, 4 and the trigger
    CHECK(out[0] == made[1]);
    CHECK(out[1] == made[2]);
    CHECK(out[2] == made[3]);
    // Touching is not overlapping.
    n = overlap_box(w, Rect{-20, -20, 16, 40}, 1, out);           // ends at x = -4, box 0 starts at -4
    CHECK(n == 0);
    // The mask selects layers; the trigger is found by a circle too.
    std::array<Entity, 8> all{};
    n = overlap_circle(w, Circle{{25, 0}, 3}, 8, all);
    REQUIRE(n == 1);
    CHECK(all[0] == other_layer);
    n = overlap_circle(w, Circle{{25, 0}, 3}, 1, all);
    CHECK(n == 3);
    CHECK(all[0] == made[2]);
    CHECK(all[1] == made[3]);
    CHECK(all[2] == trig);
    // Degenerate queries find nothing.
    CHECK(overlap_box(w, Rect{0, 0, 0, 5}, 1, out) == 0);
    CHECK(overlap_circle(w, Circle{{0, 0}, -1}, 1, out) == 0);
}

TEST_CASE("queries: bodies spawned since the last tick are found") {
    PhysicsApp p;
    World &w = p.world();
    p.ticks(1);
    const Entity fresh = spawn_kinematic(w, {500, 500}, box_collider({4, 4}));
    std::array<Entity, 2> out{};
    REQUIRE(overlap_box(w, Rect{490, 490, 20, 20}, 1, out) == 1);
    CHECK(out[0] == fresh);
    w.destroy(fresh);
    CHECK(overlap_box(w, Rect{490, 490, 20, 20}, 1, out) == 0);
}

TEST_CASE("queries: query_neighbors finds collider centres within the radius") {
    PhysicsApp p;
    World &w = p.world();
    const Entity a = spawn_static(w, {0, 0}, box_collider({2, 2}));
    const Entity b = spawn_static(w, {5, 0}, box_collider({2, 2}));
    spawn_static(w, {10, 0}, box_collider({2, 2}));               // exactly at the radius: out
    Collider off = box_collider({2, 2});
    off.offset = {-20, 0};
    const Entity c = spawn_static(w, {26, 0}, off);              // centre at 6
    p.ticks(1);
    std::array<Entity, 8> out{};
    const std::size_t n = query_neighbors(w, {0, 0}, 10, out);
    REQUIRE(n == 3);
    CHECK(out[0] == a);
    CHECK(out[1] == b);
    CHECK(out[2] == c);
}

TEST_CASE("queries: find_interactable picks the nearest in the facing cone, by kind") {
    PhysicsApp p;
    World &w = p.world();
    const auto spawn = [&](Vec2 at, StrId kind, float radius) {
        const Entity e = w.create();
        w.add<Transform>(e, Transform{at});
        w.add<Interactable>(e, Interactable{kind, radius});
        return e;
    };
    const Entity door = spawn({30, 0}, "door"_id, 4);
    const Entity chest = spawn({20, 2}, "chest"_id, 4);
    const Entity behind = spawn({-10, 0}, "door"_id, 4);
    const Entity side = spawn({0, 30}, "sign"_id, 4);
    CHECK(find_interactable(w, {0, 0}, {1, 0}, 40, 0.5f) == chest);
    CHECK(find_interactable(w, {0, 0}, {1, 0}, 40, 0.5f, "door"_id) == door);
    CHECK(find_interactable(w, {0, 0}, {-1, 0}, 40, 0.5f) == behind);
    CHECK(find_interactable(w, {0, 0}, {0, 1}, 40, 0.5f) == side);
    CHECK(find_interactable(w, {0, 0}, {1, 0}, 10, 0.5f) == Entity{});   // out of range
    CHECK(find_interactable(w, {0, 0}, {0, -1}, 40, 0.5f) == Entity{});  // nothing up there
    // Standing inside an interactable's reach: found whatever the facing.
    CHECK(find_interactable(w, {19, 2}, {0, -1}, 5, 0.1f) == chest);
}

// ---- Projectiles ---------------------------------------------------------------------------------

TEST_CASE("projectiles: a fast bullet hits a thin target it would step over and rests there") {
    PhysicsApp p;
    World &w = p.world();
    const Entity target = spawn_static(w, {100, 0}, box_collider({1, 8}));
    const Entity bullet = spawn_bullet(w, {0, 0}, {6000, 0}, Projectile{.radius = 1});
    p.ticks(1);                                                   // 100 px per tick: one step
    const auto &hits = p.events().hits;
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].e.projectile == bullet);
    CHECK(hits[0].e.target == target);
    CHECK_FALSE(hits[0].e.tile);
    CHECK(hits[0].e.point.x == doctest::Approx(98.0f));
    CHECK(hits[0].e.normal == Vec2{-1, 0});
    // Out of pierce, it stops at the contact with zero Velocity; physics never
    // destroys it, and resting there it never hits the target again.
    REQUIRE(w.alive(bullet));
    CHECK(pos_of(w, bullet).x == doctest::Approx(98.0f));
    CHECK(w.get<Velocity>(bullet).linear == Vec2{});
    CHECK(w.alive(target));
    p.ticks(5);
    CHECK(p.events().hits.size() == 1);
    CHECK(pos_of(w, bullet).x == doctest::Approx(98.0f));
}

TEST_CASE("projectiles: a PostPhysics rule reads the stopped projectile and destroys it") {
    PhysicsApp p;
    World &w = p.world();
    struct Seen {
        Vec2 position{-1, -1};
        Vec2 velocity{-1, -1};
        std::uint32_t lifetime = 0;
        bool alive = false;
    };
    w.resources().insert<Seen>();
    // The usual game rule: damage from the bullet's components, then destroy it.
    p.app().add_system(Stage::PostPhysics, [](Ctx &ctx) {
        for (const ProjectileHit &h : ctx.read<ProjectileHit>()) {
            Seen &seen = ctx.local<Seen>();
            World &world = ctx.world();
            seen.alive = world.alive(h.projectile);
            if (!seen.alive) continue;
            seen.position = world.get<Transform>(h.projectile).position;
            seen.velocity = world.get<Velocity>(h.projectile).linear;
            seen.lifetime = world.get<Lifetime>(h.projectile).ticks;
            ctx.commands().destroy(h.projectile);
        }
    }, "test.read_hits");
    spawn_static(w, {100, 0}, box_collider({1, 8}));
    const Entity bullet = spawn_bullet(w, {0, 0}, {6000, 0}, Projectile{.radius = 1});
    w.add<Lifetime>(bullet, Lifetime{90});                       // a range limit physics leaves alone
    p.ticks(1);
    const Seen &seen = w.resources().get<Seen>();
    CHECK(seen.alive);
    CHECK(seen.position.x == doctest::Approx(98.0f));
    CHECK(seen.velocity == Vec2{});
    CHECK(seen.lifetime >= 89);
    CHECK_FALSE(w.alive(bullet));                                 // the rule destroyed it
}

TEST_CASE("projectiles: pierce passes through targets in order and stops at the last") {
    PhysicsApp p;
    World &w = p.world();
    std::vector<Entity> row;
    for (int i = 0; i < 4; ++i) row.push_back(spawn_static(w, {40.0f + 20.0f * static_cast<float>(i), 0}, box_collider({2, 6})));
    const Entity bullet = spawn_bullet(w, {0, 0}, {1200, 0}, Projectile{.radius = 1, .pierce = 2});
    p.ticks(10);
    const auto &hits = p.events().hits;
    REQUIRE(hits.size() == 3);
    for (int i = 0; i < 3; ++i) CHECK(hits[static_cast<std::size_t>(i)].e.target == row[static_cast<std::size_t>(i)]);
    CHECK(hits[2].e.point.x == doctest::Approx(77.0f));            // stopped at the third
    REQUIRE(w.alive(bullet));
    CHECK(pos_of(w, bullet).x == doctest::Approx(77.0f));
    CHECK(w.get<Velocity>(bullet).linear == Vec2{});
    CHECK(w.get<Projectile>(bullet).pierce == 0);
    CHECK(w.alive(row[3]));
}

TEST_CASE("projectiles: with pierce left it flies on, spending one per target") {
    PhysicsApp p;
    World &w = p.world();
    spawn_static(w, {40, 0}, box_collider({2, 6}));
    const Entity bullet = spawn_bullet(w, {0, 0}, {1200, 0}, Projectile{.radius = 1, .pierce = 3});
    p.ticks(10);
    REQUIRE(p.events().hits.size() == 1);
    REQUIRE(w.alive(bullet));
    CHECK(w.get<Projectile>(bullet).pierce == 2);
    CHECK(pos_of(w, bullet).x == doctest::Approx(200.0f));
    CHECK(w.get<Velocity>(bullet).linear == Vec2{1200, 0});
}

TEST_CASE("projectiles: the owner, triggers and other layers are never hit") {
    PhysicsApp p;
    World &w = p.world();
    const Entity owner = spawn_static(w, {0, 0}, box_collider({8, 8}));
    Collider zone = box_collider({8, 8});
    zone.trigger = true;
    spawn_static(w, {30, 0}, zone);
    spawn_static(w, {60, 0}, box_collider({4, 4}, 2));
    const Entity target = spawn_static(w, {90, 0}, box_collider({4, 4}, 4));
    const Entity bullet = spawn_bullet(w, {0, 0}, {600, 0}, Projectile{.radius = 1, .mask = 4, .owner = owner});
    p.ticks(20);
    const auto &hits = p.events().hits;
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].e.target == target);
    CHECK(hits[0].e.projectile == bullet);
}

TEST_CASE("projectiles: tiles stop them (tile hit, no target) unless stop_at_tiles is off") {
    PhysicsApp p;
    p.tiles() = grid_from({
        "..........",
        ".....#....",
        "..........",
    });
    World &w = p.world();
    const Entity stopped = spawn_bullet(w, {8, 24}, {6000, 0}, Projectile{.radius = 2});
    const Entity ghost = spawn_bullet(w, {8, 24}, {6000, 0}, Projectile{.radius = 2, .stop_at_tiles = false});
    p.ticks(1);
    const auto &hits = p.events().hits;
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].e.projectile == stopped);
    CHECK(hits[0].e.tile);
    CHECK(hits[0].e.target == Entity{});
    CHECK(hits[0].e.point.x == doctest::Approx(78.0f));
    CHECK(hits[0].e.normal == Vec2{-1, 0});
    REQUIRE(w.alive(stopped));
    CHECK(pos_of(w, stopped).x == doctest::Approx(78.0f));
    CHECK(w.get<Velocity>(stopped).linear == Vec2{});
    CHECK(pos_of(w, ghost).x == doctest::Approx(108.0f));
    p.ticks(1);
    CHECK(pos_of(w, ghost).x == doctest::Approx(208.0f));
    CHECK(p.events().hits.size() == 1);                            // resting: no repeat
}

TEST_CASE("projectiles: a spawned bullet inside a target hits it on its first tick only") {
    PhysicsApp p;
    World &w = p.world();
    const Entity big = spawn_static(w, {0, 0}, box_collider({30, 30}));
    // Spawned through Commands, so it carries JustSpawned on its first tick.
    const Entity bullet = w.commands().spawn();
    w.commands().add<Transform>(bullet, Transform{{0, 0}});
    w.commands().add<Velocity>(bullet, Velocity{{60, 0}});
    w.commands().add<Projectile>(bullet, Projectile{.radius = 1, .pierce = 5});
    w.commands().apply();
    p.ticks(5);
    const auto &hits = p.events().hits;
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].e.target == big);
    CHECK(hits[0].tick == 1);
    // A bullet created inside it without JustSpawned (already flying through it) is not hit.
    const Entity old = spawn_bullet(w, {0, 0}, {60, 0}, Projectile{.radius = 1});
    p.ticks(5);
    CHECK(p.events().hits.size() == 1);
    (void)old;
}

TEST_CASE("projectiles: moving targets are found where they are this tick") {
    PhysicsApp p;
    World &w = p.world();
    // The target moves into the bullet's path during this tick's integrate.
    const Entity mover = spawn_kinematic(w, {100, -30}, box_collider({4, 4}), {0, 1800});
    spawn_bullet(w, {0, 0}, {12000, 0}, Projectile{.radius = 1});
    p.ticks(1);
    REQUIRE(p.events().hits.size() == 1);
    CHECK(p.events().hits[0].e.target == mover);
}

TEST_CASE("queries and projectiles: steady state does not allocate") {
    PhysicsApp p;
    p.tiles() = grid_from({
        "....................",
        "....................",
        "....................",
        "....................",
    });
    World &w = p.world();
    p.events().enabled = false;
    for (int i = 0; i < 20; ++i) spawn_static(w, {16.0f * static_cast<float>(i), 32}, box_collider({4, 4}));
    for (int i = 0; i < 10; ++i) spawn_dynamic(w, {16.0f * static_cast<float>(i), 48}, circle_collider(3));
    std::array<Entity, 16> out{};
    lam::test::run_ticks(p.app(), 120);
    std::size_t found = 0;
    CHECK_NO_ALLOC({
        for (int t = 0; t < 200; ++t) {
            found += overlap_box(w, Rect{0, 0, 300, 64}, 0xFFFFFFFFu, out);
            found += overlap_circle(w, Circle{{100, 32}, 40}, 0xFFFFFFFFu, out);
            found += query_neighbors(w, {100, 32}, 50, out);
            found += raycast(w, {0, 32}, {1, 0}, 300).has_value() ? 1u : 0u;
            found += sweep_circle(w, {0, 10}, {300, 10}, 2).has_value() ? 1u : 0u;
            found += separation(w, out[0], 20, 10).x != 0.0f ? 1u : 0u;
            lam::test::run_ticks(p.app(), 1);
        }
    });
    CHECK(found > 0);
}
