// Tile rays (line of sight), box_path_clear and the steering helpers.
#include <array>
#include <cmath>

#include <lam/physics/steering.h>
#include <lam/test/support.h>

#include "physics_app.h"
#include "physics_test_util.h"

using namespace lam;
using lam::test::feet_box;
using lam::test::grid_from;
using lam::test::open_grid;

namespace {

// Feet-anchored box sizes (width, height).
struct FootBox {
    float width;
    float height;
};
constexpr FootBox kBox{12.0f, 6.0f};
constexpr FootBox kBigBox{14.0f, 10.0f};

// Feet that put the box in the middle of the tile.
Vec2 centre(int tx, int ty, FootBox box = kBox) {
    return Vec2{static_cast<float>(tx) * 16.0f + 8.0f, static_cast<float>(ty) * 16.0f + 8.0f + box.height * 0.5f};
}
Vec2 tile_middle(int tx, int ty) { return Vec2{static_cast<float>(tx) * 16.0f + 8.0f, static_cast<float>(ty) * 16.0f + 8.0f}; }

// box_path_clear with a feet-anchored box sliding from feet a to feet b.
bool path_clear(const TileWorld &g, FootBox box, Vec2 a, Vec2 b) {
    return box_path_clear(g, feet_box(0.0f, 0.0f, box.width, box.height), a, b);
}
bool blocked_at(const TileWorld &g, FootBox box, float x, float y) {
    return box_blocked(g, feet_box(x, y, box.width, box.height));
}

} // namespace

TEST_CASE("tile rays: a ray never passes a wall") {
    const TileWorld grid = grid_from({
        ".....",
        "..#..",
        ".....",
    });
    CHECK(tile_ray_clear(grid, tile_middle(0, 0), tile_middle(4, 0)));
    CHECK(tile_ray_clear(grid, tile_middle(0, 0), tile_middle(0, 2)));
    CHECK_FALSE(tile_ray_clear(grid, tile_middle(0, 1), tile_middle(4, 1)));
    CHECK_FALSE(tile_ray_clear(grid, tile_middle(4, 1), tile_middle(0, 1)));
    CHECK_FALSE(tile_ray_clear(grid, tile_middle(1, 0), tile_middle(3, 2)));
    CHECK_FALSE(tile_ray_clear(grid, tile_middle(2, 0), tile_middle(2, 2)));
    // Ending or starting in the wall, or outside the grid, is not clear either.
    CHECK_FALSE(tile_ray_clear(grid, tile_middle(0, 0), tile_middle(2, 1)));
    CHECK_FALSE(tile_ray_clear(grid, tile_middle(2, 1), tile_middle(0, 0)));
    CHECK_FALSE(tile_ray_clear(grid, tile_middle(0, 0), Vec2{-4.0f, 8.0f}));
    CHECK_FALSE(tile_ray_clear(grid, tile_middle(0, 0), Vec2{8.0f, 1.0e9f}));
    CHECK_FALSE(tile_ray_clear(grid, tile_middle(0, 0), Vec2{std::nanf(""), 8.0f}));
    // A point is clear of itself, and a ray may graze past the wall's corner tile.
    CHECK(tile_ray_clear(grid, tile_middle(0, 0), tile_middle(0, 0)));
    CHECK(tile_ray_clear(grid, Vec2{2.0f, 20.0f}, Vec2{78.0f, 12.0f}) == false);
    CHECK(tile_ray_clear(grid, Vec2{2.0f, 2.0f}, Vec2{78.0f, 14.0f}));
    // Along the wall's face (the boundary belongs to the free tile below it).
    CHECK(tile_ray_clear(grid, Vec2{2.0f, 32.0f}, Vec2{78.0f, 32.0f}));
}

TEST_CASE("tile rays: a ray through the corner where two walls meet diagonally is blocked") {
    const TileWorld gap = grid_from({
        ".#.",
        "#..",
        "...",
    });
    CHECK_FALSE(tile_ray_clear(gap, tile_middle(0, 0), tile_middle(1, 1)));
    CHECK_FALSE(tile_ray_clear(gap, tile_middle(1, 1), tile_middle(0, 0)));
    CHECK_FALSE(tile_ray_clear(gap, tile_middle(0, 0), tile_middle(2, 2)));
    const TileWorld one = grid_from({
        ".#.",
        "...",
        "...",
    });
    CHECK(tile_ray_clear(one, tile_middle(0, 0), tile_middle(2, 2)));
    CHECK(tile_ray_clear(one, tile_middle(2, 2), tile_middle(0, 0)));
    const TileWorld other = grid_from({
        ".#.",
        "..#",
        "...",
    });
    CHECK_FALSE(tile_ray_clear(other, tile_middle(2, 0), tile_middle(1, 1)));
    CHECK_FALSE(tile_ray_clear(other, tile_middle(0, 2), tile_middle(2, 0)));
}

TEST_CASE("tile rays: tile_raycast reports the first blocking or opaque cell") {
    const TileWorld g = grid_from({
        "......",
        "..o.#.",
        "......",
    });
    const std::optional<IVec2> solid = tile_raycast(g, tile_middle(0, 1), tile_middle(5, 1));
    REQUIRE(solid.has_value());
    CHECK(*solid == IVec2{4, 1});                                // 'o' is walkable
    const std::optional<IVec2> opaque = tile_raycast(g, tile_middle(0, 1), tile_middle(5, 1), true);
    REQUIRE(opaque.has_value());
    CHECK(*opaque == IVec2{2, 1});                               // '#' alone is not opaque
    CHECK_FALSE(tile_raycast(g, tile_middle(0, 0), tile_middle(5, 0)).has_value());
    // Outside the grid is solid and opaque; with outside_solid off it is empty.
    CHECK(tile_raycast(g, tile_middle(0, 0), Vec2{-40, 8}).has_value());
    TileWorld open = g;
    open.outside_solid = false;
    CHECK_FALSE(tile_raycast(open, tile_middle(0, 0), Vec2{-40, 8}).has_value());
    CHECK(tile_ray_clear(open, Vec2{-100, -100}, Vec2{-50, 200}));
    // Any tile size and origin.
    TileWorld big = grid_from({"...", ".#.", "..."}, 32, {100, 50});
    CHECK_FALSE(tile_ray_clear(big, Vec2{116, 98}, Vec2{190, 98}));
    CHECK(tile_ray_clear(big, Vec2{116, 66}, Vec2{190, 66}));
    CHECK_FALSE(tile_ray_clear(big, Vec2{116, 66}, Vec2{196, 66}));       // x = 196 is outside
}

TEST_CASE("box path: a box slides straight only where its whole width fits") {
    const TileWorld grid = grid_from({
        "......",
        "..#...",
        "......",
        "......",
    });
    CHECK(path_clear(grid, kBox, centre(0, 0), centre(5, 0)));
    CHECK(path_clear(grid, kBox, centre(0, 2), centre(5, 3)));
    CHECK_FALSE(path_clear(grid, kBox, centre(0, 1), centre(5, 1)));
    CHECK_FALSE(path_clear(grid, kBox, centre(5, 1), centre(0, 1)));
    CHECK_FALSE(path_clear(grid, kBox, centre(2, 1), centre(2, 3)));
    CHECK_FALSE(path_clear(grid, kBox, centre(2, 3), centre(2, 1)));
    CHECK_FALSE(path_clear(grid, kBox, centre(0, 0), Vec2{-8.0f, 11.0f}));
    CHECK_FALSE(path_clear(grid, kBox, centre(0, 0), Vec2{8.0f, 1.0e9f}));
    CHECK_FALSE(path_clear(grid, kBox, Vec2{std::nanf(""), 11.0f}, centre(0, 0)));
    // The middle of the box passes the wall's corner, its edge does not.
    const Vec2 a{11.0f, 13.0f};
    const Vec2 b{51.0f, 53.0f};
    CHECK(tile_ray_clear(grid, Vec2{a.x, a.y - 1.0f}, Vec2{b.x, b.y - 1.0f}));
    CHECK_FALSE(path_clear(grid, FootBox{12.0f, 2.0f}, a, b));
    // Resting exactly against the wall and sliding along it is not touching it.
    CHECK(path_clear(grid, kBox, Vec2{20.0f, 38.0f}, Vec2{60.0f, 38.0f}));
    CHECK_FALSE(path_clear(grid, kBox, Vec2{20.0f, 37.5f}, Vec2{60.0f, 37.5f}));
    CHECK(path_clear(grid, kBox, Vec2{26.0f, 20.0f}, Vec2{26.0f, 30.0f}));
    CHECK_FALSE(path_clear(grid, kBox, Vec2{26.5f, 20.0f}, Vec2{26.5f, 30.0f}));
    CHECK(path_clear(grid, kBox, Vec2{26.0f, 20.0f}, Vec2{10.0f, 60.0f}));
    // The box sits in the middle of a tile, clear of all four neighbours.
    const TileWorld ring = grid_from({"###", "#.#", "###"});
    CHECK_FALSE(blocked_at(ring, kBigBox, centre(1, 1, kBigBox).x, centre(1, 1, kBigBox).y));
}

TEST_CASE("box path: a clear slide never overlaps a wall anywhere along it") {
    const TileWorld grid = grid_from({
        "..........",
        "..#....#..",
        "......#...",
        "...##.....",
        "........#.",
        ".#........",
        "..........",
    });
    int clear = 0;
    int blocked = 0;
    const float offsets[3] = {-2.5f, 0.0f, 3.0f};
    for (int a = 0; a < 70; ++a) {
        for (int b = 0; b < 70; ++b) {
            const float off_a = offsets[(a + b) % 3];
            const float off_b = offsets[(a * 7 + b) % 3];
            const Vec2 from{centre(a % 10, a / 10).x + off_a, centre(a % 10, a / 10).y - off_b};
            const Vec2 to{centre(b % 10, b / 10).x + off_b, centre(b % 10, b / 10).y + off_a};
            bool sampled_free = true;
            for (int s = 0; s <= 400 && sampled_free; ++s) {
                const float t = static_cast<float>(s) / 400.0f;
                sampled_free = !blocked_at(grid, kBox, from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t);
            }
            const bool answer = path_clear(grid, kBox, from, to);
            if (answer) {
                REQUIRE_MESSAGE(sampled_free, "from " << from.x << "," << from.y << " to " << to.x << "," << to.y);
                ++clear;
            } else {
                ++blocked;
            }
            if (!sampled_free) REQUIRE_FALSE(answer);
        }
    }
    CHECK(clear > 500);
    CHECK(blocked > 500);
}

TEST_CASE("steering: seek heads for the target at speed and stops on it") {
    CHECK(seek({0, 0}, {10, 0}, 5) == Vec2{5, 0});
    const Vec2 v = seek({0, 0}, {3, 4}, 10);
    CHECK(v.x == doctest::Approx(6.0f));
    CHECK(v.y == doctest::Approx(8.0f));
    CHECK(seek({1, 1}, {1, 1}, 10) == Vec2{});
    CHECK(seek({1, 1}, {1.0005f, 1}, 10) == Vec2{});
}

TEST_CASE("steering: hold_ring approaches, backs off and circles in the band between") {
    const Vec2 target{0, 0};
    // Far: toward the target.
    Vec2 v = hold_ring({100, 0}, target, 40, 30, 20);
    CHECK(v.x == doctest::Approx(-30.0f));
    // Too close (< 0.75 x 40): straight away.
    v = hold_ring({20, 0}, target, 40, 30, 20);
    CHECK(v.x == doctest::Approx(30.0f));
    CHECK(v.y == doctest::Approx(0.0f));
    // In the band: around it, clockwise on screen for +1, the other way for -1.
    v = hold_ring({35, 0}, target, 40, 30, 20, 1);
    CHECK(v.x == doctest::Approx(0.0f));
    CHECK(v.y == doctest::Approx(20.0f));
    v = hold_ring({35, 0}, target, 40, 30, 20, -1);
    CHECK(v.y == doctest::Approx(-20.0f));
    // On the target: step off to the orbit side; keep_distance <= 0 is plain seek.
    CHECK(hold_ring(target, target, 40, 30, 20, -1).x == doctest::Approx(-30.0f));
    CHECK(hold_ring({10, 0}, target, 0, 30, 20) == seek({10, 0}, target, 30));
}

TEST_CASE("steering: flee runs straight away, or turns from walls in fixed steps") {
    const TileWorld open = open_grid(20, 20);
    const Rect box = Rect::from_center({0, 0}, {4, 4});                     // relative to pos
    Vec2 v = flee(open, box, {160, 160}, {140, 160}, 50);
    CHECK(v.x == doctest::Approx(50.0f));
    CHECK(v.y == doctest::Approx(0.0f));
    // A wall 12 px ahead (column 11, x >= 176): straight on and both 45-degree turns hit it
    // within the 24 px probe, so the first clear direction is +90 degrees (down the screen).
    TileWorld walled = open_grid(20, 20);
    for (int y = 0; y < 20; ++y) walled.set({11, y}, TileShape::Solid, 0);
    v = flee(walled, box, {160, 160}, {140, 160}, 50);
    CHECK(v.x == doctest::Approx(0.0f));
    CHECK(v.y == doctest::Approx(50.0f));
    // Boxed in on every side: plain away.
    const TileWorld cell = grid_from({"###", "#.#", "###"});
    v = flee(cell, box, {24, 24}, {20, 24}, 10);
    CHECK(v.x == doctest::Approx(10.0f));
}

TEST_CASE("steering: pick_wander_goal is reproducible, in reach and reachable") {
    const TileWorld g = grid_from({
        "..........",
        "..........",
        "....##....",
        "....##....",
        "..........",
        "..........",
    });
    const Rect box = Rect::from_center({0, 0}, {4, 4});                     // relative to the anchors
    Rng a(5);
    Rng b(5);
    Vec2 ga;
    Vec2 gb;
    REQUIRE(pick_wander_goal(g, box, {40, 40}, 30, a, ga));
    REQUIRE(pick_wander_goal(g, box, {40, 40}, 30, b, gb));
    CHECK(ga == gb);
    int found = 0;
    for (int i = 0; i < 50; ++i) {
        Vec2 goal;
        if (!pick_wander_goal(g, box, {40, 40}, 30, a, goal)) continue;
        ++found;
        CHECK(distance(goal, {40, 40}) <= 30.001f);
        CHECK(box_path_clear(g, box, {40, 40}, goal));                      // straight from home
    }
    CHECK(found > 40);
    Vec2 untouched{-1, -1};
    CHECK_FALSE(pick_wander_goal(g, box, {40, 40}, 30, a, untouched, 0));
    CHECK(untouched == Vec2{-1, -1});
    // The e2d form checks the path from the body's own anchor: from the far side of the block,
    // goals round home on the near side are only reachable round it, so they are refused.
    Rng d(11);
    Rng e(11);
    Vec2 near_goal;
    CHECK(pick_wander_goal(g, box, Vec2{48, 50}, 6, d, near_goal));
    CHECK_FALSE(pick_wander_goal(g, box, Vec2{120, 50}, Vec2{48, 50}, 6, e, near_goal));
    // 4 tries by default: with every goal unreachable it spends 4 pairs of draws, and the
    // 8-attempt form spends 8.
    const TileWorld cell = grid_from({"###", "#.#", "###"});
    Rng r4(9);
    Rng r8(9);
    Rng ref4(9);
    Rng ref8(9);
    Vec2 g2;
    CHECK_FALSE(pick_wander_goal(cell, box, {24, 24}, {200, 200}, 10, r4, g2));
    CHECK_FALSE(pick_wander_goal(cell, box, {200, 200}, 10, r8, g2));
    for (int i = 0; i < 8; ++i) (void)ref4.unit();
    for (int i = 0; i < 16; ++i) (void)ref8.unit();
    CHECK(r4.next_u32() == ref4.next_u32());
    CHECK(r8.next_u32() == ref8.next_u32());
    // Shut in a one-tile cell, a goal close enough to stay inside it is found from its centre.
    Rng c(3);
    Vec2 inside;
    REQUIRE(pick_wander_goal(cell, box, {24, 24}, 3, c, inside, 16));
    CHECK(box_path_clear(cell, box, {24, 24}, inside));
}

TEST_CASE("steering: can_see needs range and no opaque tile between; sight_point is eye height") {
    const TileWorld g = grid_from({
        "..........",
        "....o.....",
        "....#.....",
        "..........",
    });
    CHECK(can_see(g, tile_middle(0, 0), tile_middle(9, 0), 200));
    CHECK_FALSE(can_see(g, tile_middle(0, 0), tile_middle(9, 0), 100));     // out of range
    CHECK_FALSE(can_see(g, tile_middle(0, 1), tile_middle(9, 1), 200));     // opaque tile between
    CHECK(can_see(g, tile_middle(0, 2), tile_middle(9, 2), 200));           // solid but see-through
    CHECK_FALSE(can_see(g, tile_middle(0, 0), Vec2{std::nanf(""), 0}, 200));
    // One pixel above the bottom centre of the body box.
    const Vec2 eye = sight_point(Rect{10, 20, 8, 16});
    CHECK(eye == Vec2{14, 35});
    CHECK(sight_point(feet_box(40, 48, 12, 6)) == Vec2{40, 47});
    // Feet exactly on a tile edge: the eye is in the tile the body stands in, so a wall tile
    // right below the feet does not block the view along the floor.
    const TileWorld floor = grid_from({
        "..........",
        "WWWWWWWWWW",
    });
    CHECK(can_see(floor, sight_point(feet_box(8, 16, 8, 8)), sight_point(feet_box(150, 16, 8, 8)), 200));
}

TEST_CASE("steering: separation pushes apart, and a stack always dissolves") {
    lam::test::PhysicsApp p;
    World &w = p.world();
    const Collider c = lam::test::box_collider({2, 2});
    const Entity s0 = lam::test::spawn_static(w, {10, 10}, c);
    const Entity s1 = lam::test::spawn_static(w, {10, 10}, c);
    const Entity s2 = lam::test::spawn_static(w, {10, 10}, c);
    const Entity n0 = lam::test::spawn_static(w, {100, 100}, c);
    lam::test::spawn_static(w, {104, 100}, c);
    const Entity f0 = lam::test::spawn_static(w, {300, 300}, c);
    lam::test::spawn_static(w, {340, 300}, c);
    const Vec2 a = separation(w, s0, 8.0f, 30.0f);
    const Vec2 m = separation(w, s1, 8.0f, 30.0f);
    const Vec2 z = separation(w, s2, 8.0f, 30.0f);
    CHECK(a.x < 0.0f);
    CHECK(z.x > 0.0f);
    CHECK(m.x == doctest::Approx(0.0f));
    CHECK(m.y != 0.0f);
    CHECK(length(a) <= 30.001f);
    CHECK(separation(w, n0, 8.0f, 30.0f).x < 0.0f);
    CHECK(separation(w, f0, 8.0f, 30.0f) == Vec2{});
    CHECK(separation(w, n0, 0.0f, 30.0f) == Vec2{});
}
