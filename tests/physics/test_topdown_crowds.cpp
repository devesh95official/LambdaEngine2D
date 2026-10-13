// Top-down bodies that start inside a wall walk out of it, and crowd queries (which keep only the
// candidates within the radius before sorting) stay bit-identical.
#include <doctest/doctest.h>

#include <lam/ecs/transform.h>
#include <lam/physics/motors.h>
#include <lam/physics/queries.h>
#include <lam/physics/tile_world.h>

#include "physics_app.h"
#include "physics_test_util.h"

using namespace lam;
using lam::test::box_collider;
using lam::test::grid_from;
using lam::test::PhysicsApp;
using lam::test::pos_of;

TEST_CASE("a top-down motor body spawned inside a wall walks out of it") {
    PhysicsApp a;
    World &w = a.world();
    a.tiles() = grid_from({
        "########",
        "#......#",
        "#......#",
        "#......#",
        "########",
    });
    const Entity e = w.create();
    w.add<Transform>(e, Transform{{40, 18}});                   // box y 15..21: 1 px into the top wall
    w.add<Collider>(e, box_collider({5, 3}));
    w.add<TopDownMotor>(e).move = {0, 1};
    a.ticks(10);
    CHECK(pos_of(w, e).y > 18.0f);
    CHECK_FALSE(box_blocked(a.tiles(), collider_box(w.get<Collider>(e), pos_of(w, e))));
    // Pushing into the wall from inside it does nothing.
    w.get<Transform>(e).position = {40, 18};
    w.get<Velocity>(e).linear = {};
    w.get<TopDownMotor>(e).move = {0, -1};
    a.ticks(5);
    CHECK(pos_of(w, e).y == 18.0f);
    // Sliding along the wall while inside it keeps the same overlap: allowed.
    w.get<Velocity>(e).linear = {};
    w.get<TopDownMotor>(e).move = {1, 0};
    a.ticks(5);
    CHECK(pos_of(w, e).x > 40.0f);
    CHECK(pos_of(w, e).y == 18.0f);
}

TEST_CASE("escaping from inside a tile never enters another one") {
    TileWorld tw = grid_from({
        "....",
        ".##.",
        "....",
    });
    const Rect box{20, 14, 8, 8};                               // overlaps the left block's top row
    const BoxMove left = move_box(tw, box, {-6, 0});
    CHECK(left.box.x == 14.0f);                                 // out to the left: less overlap
    const BoxMove right = move_box(tw, box, {6, 0});
    CHECK(right.box.x == 26.0f);                                // along the two blocks: same overlap
    const BoxMove down = move_box(tw, box, {0, 6});
    CHECK(down.box.y == 14.0f);                                 // deeper: refused
    CHECK(down.hit_y);
}

TEST_CASE("separation over a dense crowd is the same float sum") {
    PhysicsApp a;
    World &w = a.world();
    std::vector<Entity> crowd;
    for (int i = 0; i < 400; ++i) {
        const Entity e = w.create();
        const float ang = static_cast<float>(i) * 2.399963f;
        const float rad = 3.0f * std::sqrt(static_cast<float>(i));
        w.add<Transform>(e, Transform{{200 + rad * std::cos(ang), 200 + rad * std::sin(ang)}});
        w.add<Collider>(e, box_collider({3, 3}, i % 3 == 0 ? 2u : 1u));
        crowd.push_back(e);
    }
    a.ticks(1);
    // Reference: the plain O(n) sum in index order over every body within the radius.
    const auto reference = [&](Entity self, float radius, float strength, CollisionMask mask) {
        const Vec2 me = pos_of(w, self) + w.get<Collider>(self).offset;
        Vec2 push;
        int stacked = 0, across = 0;
        for (const Entity e : crowd) {
            if (e == self || (w.get<Collider>(e).layer & mask) == 0) continue;
            const Vec2 delta = me - (pos_of(w, e) + w.get<Collider>(e).offset);
            const float d2 = length_sq(delta);
            if (!(d2 < radius * radius)) continue;
            const float d = std::sqrt(d2);
            if (d < 0.001f) {
                ++stacked;
                across += self.index < e.index ? -1 : 1;
                continue;
            }
            push += delta * (strength * (1.0f - d / radius) / d);
        }
        if (stacked > 0) {
            const float x = static_cast<float>(across) / static_cast<float>(stacked);
            const float upright = std::sqrt(std::max(0.0f, 1.0f - x * x));
            push.x += x * strength;
            push.y += (self.index % 2 == 0 ? upright : -upright) * strength;
        }
        return clamp_length(push, strength);
    };
    for (std::size_t i = 0; i < crowd.size(); i += 7) {
        const Vec2 got = separation(w, crowd[i], 12.0f, 40.0f, 1u);
        const Vec2 want = reference(crowd[i], 12.0f, 40.0f, 1u);
        CHECK(got.x == want.x);
        CHECK(got.y == want.y);
    }
}
