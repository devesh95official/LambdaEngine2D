// move_box in platformer mode: slopes, one-way tiles, entity platforms, ground snap and ceilings.
#include <cmath>
#include <vector>

#include <lam/physics/tile_world.h>
#include <lam/test/support.h>

#include "physics_test_util.h"

using namespace lam;
using lam::test::grid_from;

namespace {

MoveBoxOptions platformer(float snap = 0.0f) {
    MoveBoxOptions o;
    o.platformer = true;
    o.snap_down = snap;
    return o;
}

// An 8 x 12 body with its bottom-centre ("foot") at (x, bottom).
Rect body(float x, float bottom) { return Rect{x - 4.0f, bottom - 12.0f, 8.0f, 12.0f}; }

float foot_y(const BoxMove &r) { return r.box.y + r.box.h; }

} // namespace

TEST_CASE("platformer sweep: the foot lands on a slope's surface under the box's centre") {
    const TileWorld g = grid_from({
        "........",
        "........",
        "...../..",
        "########",
    });
    // The '/' tile spans x 80..96, y 32..48; at x = 88 its surface is at y = 40.
    const BoxMove r = move_box(g, body(88, 20), {0, 60}, platformer());
    CHECK(r.grounded);
    CHECK(r.on_slope);
    CHECK(foot_y(r) == 40.0f);
    CHECK(r.ground_normal.x == doctest::Approx(-0.70710678f));
    CHECK(r.ground_normal.y == doctest::Approx(-0.70710678f));
    // Flat ground: normal straight up, not a slope.
    const BoxMove flat = move_box(g, body(24, 20), {0, 60}, platformer());
    CHECK(flat.grounded);
    CHECK_FALSE(flat.on_slope);
    CHECK(foot_y(flat) == 48.0f);
    CHECK(flat.ground_normal == Vec2{0, -1});
}

TEST_CASE("platformer sweep: ground snap keeps a walking box on a slope going down") {
    const TileWorld g = grid_from({
        "........",
        "........",
        "...../..",
        "########",
    });
    const Rect standing = body(88, 40);
    // Walking left (downhill) by 3 px: the surface drops 3 px.
    const BoxMove snapped = move_box(g, standing, {-3, 0}, platformer(4.0f));
    CHECK(snapped.grounded);
    CHECK(foot_y(snapped) == 43.0f);
    const BoxMove floating = move_box(g, standing, {-3, 0}, platformer(0.0f));
    CHECK_FALSE(floating.grounded);
    CHECK(foot_y(floating) == 40.0f);
    // Walking right (uphill) the foot rises with the surface, snap or not.
    const BoxMove up = move_box(g, standing, {3, 0}, platformer());
    CHECK(up.grounded);
    CHECK(foot_y(up) == 37.0f);
}

TEST_CASE("platformer sweep: one-way tiles hold only a box that comes from above") {
    const TileWorld g = grid_from({
        "......",
        "......",
        "..==..",
        "......",
        "######",
    });
    // From above: lands on the top (y = 32).
    BoxMove r = move_box(g, body(40, 20), {0, 30}, platformer());
    CHECK(r.grounded);
    CHECK(foot_y(r) == 32.0f);
    // From below: passes up through it.
    r = move_box(g, body(40, 60), {0, -30}, platformer());
    CHECK_FALSE(r.hit_y);
    CHECK(foot_y(r) == 30.0f);
    // A box whose foot is already below the top (still passing up) falls back through it.
    r = move_box(g, body(40, 40), {0, 10}, platformer());
    CHECK_FALSE(r.grounded);
    CHECK(foot_y(r) == 50.0f);
    // drop_through ignores it.
    MoveBoxOptions drop = platformer();
    drop.drop_through = true;
    r = move_box(g, body(40, 32), {0, 10}, drop);
    CHECK(foot_y(r) == 42.0f);
    // Top-down mode never sees one-way tiles.
    r = move_box(g, body(40, 20), {0, 30});
    CHECK(foot_y(r) == 50.0f);
}

TEST_CASE("platformer sweep: entity solids and platforms report which one is the ground") {
    const TileWorld g = grid_from({
        "..........",
        "..........",
        "..........",
        "..........",
        "..........",
        "##########",
    });
    const std::vector<Rect> solids{Rect{100, 60, 30, 10}, Rect{20, 50, 30, 10}};
    const std::vector<Rect> planks{Rect{60, 40, 30, 4}};
    MoveBoxOptions o = platformer();
    o.solids = solids;
    o.one_way = planks;
    BoxMove r = move_box(g, body(35, 30), {0, 40}, o);
    CHECK(r.grounded);
    CHECK(foot_y(r) == 50.0f);
    CHECK(r.ground_solid == 1);
    r = move_box(g, body(75, 30), {0, 40}, o);
    CHECK(foot_y(r) == 40.0f);
    CHECK(r.ground_solid == 2);                                  // solids first, then one_way
    // From below the plank does not hold.
    r = move_box(g, body(75, 60), {0, -30}, o);
    CHECK_FALSE(r.hit_y);
    // A solid blocks sideways and reports blocking_solid.
    r = move_box(g, body(90, 70), {30, 0}, o);
    CHECK(r.hit_x);
    CHECK(r.blocking_solid == 0);
    CHECK(r.box.x + r.box.w <= 100.0f);
    // Tiles beat nothing: ground on the floor reports no solid.
    r = move_box(g, body(150, 70), {0, 40}, o);
    CHECK(r.grounded);
    CHECK(r.ground_solid == -1);
}

TEST_CASE("platformer sweep: ceilings stop a rising box flush; huge falls never tunnel") {
    const TileWorld g = grid_from({
        "######",
        "......",
        "......",
        "......",
        "######",
    });
    BoxMove r = move_box(g, body(40, 60), {0, -100}, platformer());
    CHECK(r.hit_y);
    CHECK(r.box.y == 16.0f);
    for (const float fall : {34.0f, 1000.0f, 1.0e6f}) {
        r = move_box(g, body(40, 30), {0, fall}, platformer());
        CHECK(r.grounded);
        CHECK(foot_y(r) == 64.0f);
    }
    // A box inside a wall stays where it is and reports both axes.
    r = move_box(g, body(40, 8), {5, 5}, platformer());
    CHECK(r.hit_x);
    CHECK(r.hit_y);
    CHECK(r.box == body(40, 8));
    // Non-finite moves do nothing.
    r = move_box(g, body(40, 60), {std::nanf(""), 1}, platformer());
    CHECK(r.box == body(40, 60));
}
