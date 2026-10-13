// TileWorld edges: an empty TileWorld, set() with a path cost, and open sides (cells beyond an
// open side are Empty for every test: sweeps, rays, box paths).
#include <doctest/doctest.h>

#include <lam/physics/queries.h>
#include <lam/physics/steering.h>
#include <lam/physics/tile_world.h>

#include "detail/tile_math.h"
#include "physics_test_util.h"

using namespace lam;
using lam::test::grid_from;

TEST_CASE("tile world: an empty TileWorld answers Empty and no flags everywhere") {
    TileWorld tw;
    CHECK(tw.outside_solid);
    CHECK_FALSE(tw.has_cells());
    CHECK(tw.at({0, 0}) == TileShape::Empty);
    CHECK(tw.at({-5, 7}) == TileShape::Empty);
    CHECK(tw.flags_at({0, 0}) == 0);
    CHECK_FALSE(tw.opaque({3, -2}));
    tw.resize(2, 2, 16, {});
    CHECK(tw.has_cells());
    CHECK(tw.at({-1, 0}) == TileShape::Solid);
    CHECK(tw.opaque({2, 0}));
}

TEST_CASE("tile world: TileWorld::set with a path cost writes the cost, one version bump") {
    TileWorld tw;
    tw.resize(3, 2, 16, {});
    CHECK(tw.cost(1, 1) == kDefaultTileCost);                    // the default lives here
    const std::uint32_t v = tw.version;
    tw.set({1, 1}, TileShape::Solid, kTileOpaque, 0);
    CHECK(tw.version == v + 1);
    CHECK(tw.at({1, 1}) == TileShape::Solid);
    CHECK(tw.flags_at({1, 1}) == kTileOpaque);
    CHECK(tw.cost(1, 1) == 0);
    tw.set({1, 1}, TileShape::Empty, 0);                         // the overload without a cost keeps it
    CHECK(tw.cost(1, 1) == 0);
    tw.set({5, 5}, TileShape::Solid, 0, 3);                      // outside: no-op
    CHECK(tw.version == v + 2);
}

TEST_CASE("tile world: open_sides makes cells beyond those sides Empty") {
    TileWorld tw = grid_from({
        "#...#",
        "#...#",
        "#...#",
    });
    tw.open_sides = kSideBottom;
    CHECK(tw.at({2, 3}) == TileShape::Empty);                    // below: open
    CHECK(tw.flags_at({2, 3}) == 0);
    CHECK(tw.at({-1, 1}) == TileShape::Solid);                   // left: closed
    CHECK(tw.opaque({5, 1}));
    CHECK(tw.at({2, -1}) == TileShape::Solid);                   // top: closed
    CHECK(tw.at({-1, 3}) == TileShape::Empty);                   // a corner beyond an open side
    tw.outside_solid = false;
    CHECK(tw.at({-1, 1}) == TileShape::Empty);                   // outside_solid off: all open
}

TEST_CASE("tile world: a pit with an open bottom has no invisible floor") {
    TileWorld tw = grid_from({
        "#...#",
        "#...#",
        "#...#",
    });
    const Rect box{24, 20, 12, 12};
    // Closed (the default): the bottom edge is a floor.
    BoxMove m = move_box(tw, box, {0, 200});
    CHECK(m.hit_y);
    CHECK(m.box.y + m.box.h == doctest::Approx(48.0f));
    m = move_box(tw, box, {0, 200}, MoveBoxOptions{.platformer = true});
    CHECK(m.grounded);
    CHECK(m.box.y + m.box.h == doctest::Approx(48.0f));
    CHECK_FALSE(box_path_clear(tw, Rect{0, 0, 12, 12}, {24, 20}, {24, 220}));
    CHECK_FALSE(tile_ray_clear(tw, {40, 20}, {40, 400}));
    // Open bottom: the box falls out of the grid; the walls still hold.
    tw.open_sides = kSideBottom;
    m = move_box(tw, box, {0, 200});
    CHECK_FALSE(m.hit_y);
    CHECK(m.box.y == doctest::Approx(220.0f));
    m = move_box(tw, box, {0, 200}, MoveBoxOptions{.platformer = true});
    CHECK_FALSE(m.grounded);
    CHECK(m.box.y == doctest::Approx(220.0f));
    m = move_box(tw, box, {-100, 0});
    CHECK(m.hit_x);
    CHECK(m.box.x == doctest::Approx(16.0f));
    CHECK(box_path_clear(tw, Rect{0, 0, 12, 12}, {24, 20}, {24, 220}));
    CHECK_FALSE(box_path_clear(tw, Rect{0, 0, 12, 12}, {24, 20}, {-80, 20}));   // out through the left
    CHECK(tile_ray_clear(tw, {40, 20}, {40, 400}));
    CHECK(tile_ray_clear(tw, {40, 400}, {40, 20}));                               // and back in
    CHECK_FALSE(tile_ray_clear(tw, {40, 20}, {-400, 20}));
    CHECK(tile_raycast(tw, {40, 20}, {40, 400}, true) == std::nullopt);
    REQUIRE(tile_raycast(tw, {40, 20}, {-400, 20}).has_value());
    CHECK(tile_raycast(tw, {40, 20}, {-400, 20})->x == 0);
}

TEST_CASE("tile world: swept circles leave through an open side and stop at a closed one") {
    TileWorld tw = grid_from({
        "#...#",
        "#...#",
        "#...#",
    });
    tw.open_sides = kSideBottom;
    CHECK_FALSE(detail::sweep_circle_tiles(tw, {40, 20}, {40, 400}, 3).has_value());
    // Coming up from below the pit is free until the circle reaches a tile.
    CHECK_FALSE(detail::sweep_circle_tiles(tw, {40, 400}, {40, 30}, 3).has_value());
    // Beyond a closed side the outside is solid: a start there is a start inside.
    const auto inside = detail::sweep_circle_tiles(tw, {-30, 20}, {-30, 10}, 3);
    REQUIRE(inside.has_value());
    CHECK(inside->t == 0.0f);
    CHECK(inside->normal == Vec2{});
    CHECK_FALSE(detail::sweep_circle_tiles(tw, {-30, 20}, {-30, 10}, 3, true).has_value());
    // Through the left wall: the wall tile stops it.
    const auto wall = detail::sweep_circle_tiles(tw, {40, 20}, {-200, 20}, 3);
    REQUIRE(wall.has_value());
    CHECK(wall->cell == IVec2{0, 1});
    // Closed bottom: the circle stops at the outside below the grid.
    tw.open_sides = 0;
    const auto floor = detail::sweep_circle_tiles(tw, {40, 20}, {40, 400}, 3);
    REQUIRE(floor.has_value());
    CHECK(floor->point.y == doctest::Approx(45.0f));
}

TEST_CASE("tile world: corners beyond an open side are Empty for sweeps and box paths too") {
    TileWorld tw = grid_from({
        "#...#",
        "#...#",
        "#...#",
    });
    tw.open_sides = kSideBottom;
    REQUIRE(tw.at({-3, 5}) == TileShape::Empty);                 // below the grid and left of it
    // A box wholly below the open bottom may slide past the closed left side's line.
    CHECK(box_path_clear(tw, Rect{0, 0, 12, 12}, {24, 100}, {-80, 100}));
    // ...but not while any part of its path is level with the grid's rows.
    CHECK_FALSE(box_path_clear(tw, Rect{0, 0, 12, 12}, {24, 40}, {-80, 40}));
    // A circle starting there is not inside the solid outside.
    CHECK_FALSE(detail::sweep_circle_tiles(tw, {-30, 100}, {-30, 90}, 3).has_value());
    tw.open_sides = 0;
    CHECK_FALSE(box_path_clear(tw, Rect{0, 0, 12, 12}, {24, 100}, {-80, 100}));
    CHECK(detail::sweep_circle_tiles(tw, {-30, 100}, {-30, 90}, 3).has_value());
}
