// A* path finding, in cell units: cell (x, y) spans [x, x+1) x [y, y+1).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <lam/core/data.h>
#include <lam/core/hash.h>
#include <lam/ecs/component.h>
#include <lam/math/rng.h>
#include <lam/physics/tile_world.h>
#include <lam/test/support.h>
#include <lam/tilemap/grid_maps.h>
#include <lam/tilemap/grid_path.h>
#include <lam/tilemap/path_follow.h>

using namespace lam;

namespace {

using CostGrid = Grid<std::uint16_t>;

// '#' blocked, '~' swamp (cost 40), digits d: cost d * 10, anything else cost 10.
CostGrid grid_from(std::initializer_list<const char *> rows) {
    const std::string first = *rows.begin();
    CostGrid g(static_cast<int>(first.size()), static_cast<int>(rows.size()), 10);
    int y = 0;
    for (const char *row : rows) {
        const std::string text = row;
        REQUIRE(static_cast<int>(text.size()) == g.width());
        for (int x = 0; x < g.width(); ++x) {
            const char c = text[static_cast<std::size_t>(x)];
            std::uint16_t v = 10;
            if (c == '#')
                v = 0;
            else if (c == '~')
                v = 40;
            else if (c >= '1' && c <= '9')
                v = static_cast<std::uint16_t>((c - '0') * 10);
            g(x, y) = v;
        }
        ++y;
    }
    return g;
}

CostGrid open_grid(int w, int h) {
    return CostGrid(w, h, 10);
}

PathFinder finder_for(const CostGrid &g, int searches = 100) {
    PathFinder f;
    f.resize(g.width() * g.height());
    f.begin_tick(searches);
    return f;
}

Vec2 mid(int x, int y) {
    return cell_center({x, y});
}

// Does a box (centre c, half extents h) overlap a blocked cell (or leave the grid)?
bool box_blocked(const CostGrid &g, Vec2 c, Vec2 h) {
    const int x0 = static_cast<int>(std::floor(c.x - h.x));
    const int x1 = static_cast<int>(std::ceil(c.x + h.x)) - 1;
    const int y0 = static_cast<int>(std::floor(c.y - h.y));
    const int y1 = static_cast<int>(std::ceil(c.y + h.y)) - 1;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            // strict overlap: the box interior meets the cell interior
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            if (!(c.x - h.x < fx + 1 && fx < c.x + h.x && c.y - h.y < fy + 1 && fy < c.y + h.y))
                continue;
            if (g.get_or(x, y, 0) == 0) return true;
        }
    }
    return false;
}

// Slides the box along every leg (start cell centre, then each waypoint centre) in small steps.
bool route_free_of_walls(const CostGrid &g, IVec2 from, const PathRoute &route,
                         Vec2 half = {0.4999f, 0.4999f}) {
    Vec2 a = cell_center(from);
    for (int i = 0; i < route.count; ++i) {
        const Vec2 b = cell_center(route.cells[static_cast<std::size_t>(i)]);
        const int steps = static_cast<int>(distance(a, b) * 64.0f) + 1;
        for (int s = 0; s <= steps; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            if (box_blocked(g, lerp(a, b, t), half)) return false;
        }
        a = b;
    }
    return true;
}

float route_length(IVec2 from, const PathRoute &route) {
    float total = 0;
    Vec2 a = cell_center(from);
    for (int i = 0; i < route.count; ++i) {
        const Vec2 b = cell_center(route.cells[static_cast<std::size_t>(i)]);
        total += distance(a, b);
        a = b;
    }
    return total;
}

TileWorld unit_world(const CostGrid &g) {
    TileWorld tw;
    tw.resize(g.width(), g.height(), 1, {0, 0});
    return tw; // tile_center(cell) == cell_center(cell)
}

// Moves a box one axis at a time, stopping each axis at blocked cells (a tiny stand-in for
// physics move_box).
Vec2 slide(const CostGrid &g, Vec2 p, Vec2 d, Vec2 half) {
    const Vec2 x{p.x + d.x, p.y};
    if (!box_blocked(g, x, half)) p = x;
    const Vec2 y{p.x, p.y + d.y};
    if (!box_blocked(g, y, half)) p = y;
    return p;
}

// Walks a box along routes until it reaches the goal cell centre, asking again whenever the route
// is stale (2 searches a tick, repath every 20 ticks). Returns the ticks used, -1 if it never
// arrived.
int walk_to(const CostGrid &g, Vec2 from, IVec2 goal, int max_ticks, Vec2 half,
            float speed = 0.1f) {
    PathFinder finder;
    finder.resize(g.width() * g.height());
    const TileWorld tw = unit_world(g);
    PathRoute route;
    Vec2 pos = from;
    for (int tick = 0; tick < max_ticks; ++tick) {
        finder.begin_tick(2);
        if (route_stale(route, goal, static_cast<std::uint64_t>(tick), route.computed_tick, 20)) {
            const Result<PathStatus> s =
                finder.find(g, floor_to_ivec(pos), goal, GridDirs::Eight, route);
            REQUIRE(s.ok());
            if (*s != PathStatus::NoBudget) route.computed_tick = static_cast<std::uint64_t>(tick);
        }
        const Vec2 dir = follow_route(route, tw, pos, 0.05f);
        pos = slide(g, pos, dir * speed, half);
        if (distance(pos, cell_center(goal)) <= 0.06f) return tick;
    }
    return -1;
}

std::uint32_t step_cost(const CostGrid &g, IVec2 from, IVec2 to) {
    const std::uint32_t c = g(to.x, to.y);
    return (from.x != to.x && from.y != to.y) ? (c * 14u + 5u) / 10u : c;
}

} // namespace

// ---- basics -------------------------------------------------------------------------------------

TEST_CASE("grid path: a position stands on the cell under it and a cell has one centre") {
    TileWorld tw;
    tw.resize(10, 10, 16, {0, 0});
    CHECK(tw.tile_at({40, 51}) == IVec2{2, 3});
    CHECK(tw.tile_at({40, 48}) == IVec2{2, 3});
    CHECK(tw.tile_at({40, 47.99f}) == IVec2{2, 2});
    CHECK(tw.tile_at({-1, 2}) == IVec2{-1, 0});
    CHECK(tw.tile_center({2, 3}) == Vec2{40, 56});
    CHECK(tw.tile_at(tw.tile_center({2, 3})) == IVec2{2, 3});
    // A shifted origin and another tile size (runtime tile size).
    TileWorld big;
    big.resize(4, 4, 32, {100, -50});
    CHECK(big.tile_at({100, -50}) == IVec2{0, 0});
    CHECK(big.tile_center({1, 1}) == Vec2{148, -2});
    // The box at a cell centre sits clear of all four neighbours.
    const CostGrid ring = grid_from({"###", "#.#", "###"});
    CHECK_FALSE(box_blocked(ring, mid(1, 1), {0.5f, 0.5f}));
    CHECK(box_blocked(ring, mid(1, 1) + Vec2{0.01f, 0}, {0.5f, 0.5f}));
}

TEST_CASE("grid path: a ray never passes a wall") {
    const CostGrid g = grid_from({".....", "..#..", "....."});
    CHECK(grid_ray_clear(g, mid(0, 0), mid(4, 0)));
    CHECK(grid_ray_clear(g, mid(0, 0), mid(0, 2)));
    CHECK_FALSE(grid_ray_clear(g, mid(0, 1), mid(4, 1)));
    CHECK_FALSE(grid_ray_clear(g, mid(4, 1), mid(0, 1)));
    CHECK_FALSE(grid_ray_clear(g, mid(1, 0), mid(3, 2)));
    CHECK_FALSE(grid_ray_clear(g, mid(2, 0), mid(2, 2)));
    // Ending or starting in the wall, or outside the grid, is not clear either.
    CHECK_FALSE(grid_ray_clear(g, mid(0, 0), mid(2, 1)));
    CHECK_FALSE(grid_ray_clear(g, mid(2, 1), mid(0, 0)));
    CHECK_FALSE(grid_ray_clear(g, mid(0, 0), Vec2{-0.25f, 0.5f}));
    CHECK_FALSE(grid_ray_clear(g, mid(0, 0), Vec2{0.5f, 6.25e7f}));
    CHECK_FALSE(grid_ray_clear(g, mid(0, 0), Vec2{std::nanf(""), 0.5f}));
    // A point is clear of itself, and a ray may graze past the wall's corner cell.
    CHECK(grid_ray_clear(g, mid(0, 0), mid(0, 0)));
    CHECK_FALSE(grid_ray_clear(g, Vec2{0.125f, 1.25f}, Vec2{4.875f, 0.75f}));
    CHECK(grid_ray_clear(g, Vec2{0.125f, 0.125f}, Vec2{4.875f, 0.875f}));
    // Along the wall's face (the boundary belongs to the free cell below it).
    CHECK(grid_ray_clear(g, Vec2{0.125f, 2.0f}, Vec2{4.875f, 2.0f}));
}

TEST_CASE("grid path: a ray through the corner where two walls meet diagonally is blocked") {
    const CostGrid gap = grid_from({".#.", "#..", "..."});
    CHECK_FALSE(grid_ray_clear(gap, mid(0, 0), mid(1, 1)));
    CHECK_FALSE(grid_ray_clear(gap, mid(1, 1), mid(0, 0)));
    CHECK_FALSE(grid_ray_clear(gap, mid(0, 0), mid(2, 2)));
    const CostGrid one = grid_from({".#.", "...", "..."});
    CHECK(grid_ray_clear(one, mid(0, 0), mid(2, 2)));
    CHECK(grid_ray_clear(one, mid(2, 2), mid(0, 0)));
    const CostGrid other = grid_from({".#.", "..#", "..."});
    CHECK_FALSE(grid_ray_clear(other, mid(2, 0), mid(1, 1)));
    CHECK_FALSE(grid_ray_clear(other, mid(0, 2), mid(2, 0)));
}

TEST_CASE("grid path: a box slides straight only where its whole width fits") {
    const CostGrid g = grid_from({"......", "..#...", "......", "......"});
    // A 12x6 px foot box on 16 px tiles, as a centred box in cell units.
    const Vec2 half{0.375f, 0.1875f};
    CHECK(grid_box_clear(g, mid(0, 0), mid(5, 0), half));
    CHECK(grid_box_clear(g, mid(0, 2), mid(5, 3), half));
    CHECK_FALSE(grid_box_clear(g, mid(0, 1), mid(5, 1), half));
    CHECK_FALSE(grid_box_clear(g, mid(5, 1), mid(0, 1), half));
    CHECK_FALSE(grid_box_clear(g, mid(2, 1), mid(2, 3), half));
    CHECK_FALSE(grid_box_clear(g, mid(2, 3), mid(2, 1), half));
    CHECK_FALSE(grid_box_clear(g, mid(0, 0), Vec2{-0.5f, 0.5f}, half));
    CHECK_FALSE(grid_box_clear(g, mid(0, 0), Vec2{0.5f, 6.25e7f}, half));
    CHECK_FALSE(grid_box_clear(g, Vec2{std::nanf(""), 0.5f}, mid(0, 0), half));

    // The middle of the box passes the wall's corner, its edge does not.
    const Vec2 a{11.0f / 16, 12.0f / 16};
    const Vec2 b{51.0f / 16, 52.0f / 16};
    CHECK(grid_ray_clear(g, a, b));
    CHECK_FALSE(grid_box_clear(g, a, b, {6.0f / 16, 1.0f / 16}));

    // Resting exactly against the wall and sliding along it is not touching it.
    CHECK(grid_box_clear(g, {20.0f / 16, 35.0f / 16}, {60.0f / 16, 35.0f / 16}, half));
    CHECK_FALSE(grid_box_clear(g, {20.0f / 16, 34.5f / 16}, {60.0f / 16, 34.5f / 16}, half));
    CHECK(grid_box_clear(g, {26.0f / 16, 17.0f / 16}, {26.0f / 16, 27.0f / 16}, half));
    CHECK_FALSE(grid_box_clear(g, {26.5f / 16, 17.0f / 16}, {26.5f / 16, 27.0f / 16}, half));
    // ...and so is leaving the wall at a slant.
    CHECK(grid_box_clear(g, {26.0f / 16, 17.0f / 16}, {10.0f / 16, 57.0f / 16}, half));
    // A costlier cell blocks a box limited to cheaper ones.
    const CostGrid swamp = grid_from({"....", ".~..", "...."});
    CHECK(grid_box_clear(swamp, mid(0, 1), mid(3, 1), {0.4f, 0.4f}));
    CHECK_FALSE(grid_box_clear(swamp, mid(0, 1), mid(3, 1), {0.4f, 0.4f}, 10));
}

TEST_CASE("grid path: a clear slide never overlaps a wall anywhere along it") {
    const CostGrid g = grid_from({"..........", "..#....#..", "......#...", "...##.....",
                                  "........#.", ".#........", ".........."});
    const Vec2 half{0.375f, 0.1875f};
    int clear = 0;
    int blocked = 0;
    const float offsets[3] = {-2.5f / 16, 0.0f, 3.0f / 16};
    for (int a = 0; a < 70; ++a) {
        for (int b = 0; b < 70; ++b) {
            const float oa = offsets[(a + b) % 3];
            const float ob = offsets[(a * 7 + b) % 3];
            const Vec2 from{mid(a % 10, a / 10).x + oa, mid(a % 10, a / 10).y - ob};
            const Vec2 to{mid(b % 10, b / 10).x + ob, mid(b % 10, b / 10).y + oa};
            bool sampled_free = true;
            for (int s = 0; s <= 400 && sampled_free; ++s) {
                sampled_free =
                    !box_blocked(g, lerp(from, to, static_cast<float>(s) / 400.0f), half);
            }
            const bool answer = grid_box_clear(g, from, to, half);
            if (answer) {
                CHECK_MESSAGE(sampled_free,
                              "from " << from.x << "," << from.y << " to " << to.x << "," << to.y);
                ++clear;
            } else {
                ++blocked;
            }
            if (!sampled_free) CHECK_FALSE(answer);
        }
    }
    CHECK(clear > 500);
    CHECK(blocked > 500);
}

TEST_CASE("grid path: a goal in plain view is one waypoint and costs no search") {
    const CostGrid g = open_grid(12, 8);
    PathFinder finder = finder_for(g, 1);
    PathRoute route;
    route.computed_tick = 77;
    REQUIRE(finder.find(g, {1, 1}, {9, 6}, GridDirs::Eight, route).value() == PathStatus::Found);
    CHECK(route.count == 1);
    CHECK(route.cells[0] == IVec2{9, 6});
    CHECK(route.complete);
    CHECK(route.next == 0);
    CHECK(route.goal == IVec2{9, 6});
    CHECK(route.computed_tick == 77); // the caller's to stamp
    CHECK(finder.searches_left() == 1);
    CHECK(finder.last_expansions() == 0);

    // The same cell, even walled in and with no budget.
    const CostGrid cell = grid_from({"###", "#.#", "###"});
    PathFinder small = finder_for(cell, 0);
    REQUIRE(small.find(cell, {1, 1}, {1, 1}, GridDirs::Eight, route).value() == PathStatus::Found);
    CHECK(route.count == 1);
    CHECK(route.cells[0] == IVec2{1, 1});
}

TEST_CASE("grid path: the shortest way round a wall") {
    const CostGrid g = grid_from({".........", "....#....", "....#....", "....#....", "....#....",
                                  "....#....", "....#....", "....#....", "........."});
    PathFinder finder = finder_for(g);
    PathRoute route;
    const IVec2 from{1, 3};
    REQUIRE(finder.find(g, from, {7, 3}, GridDirs::Eight, route).value() == PathStatus::Found);
    CHECK(route.complete);
    CHECK(route.cells[static_cast<std::size_t>(route.count - 1)] == IVec2{7, 3});
    CHECK(route_free_of_walls(g, from, route));
    for (int i = 0; i < route.count; ++i)
        CHECK(route.cells[static_cast<std::size_t>(i)].y <= from.y);
    CHECK(route.count <= 3);
    // No longer than the cell path (2 diagonals, 3 straight, 2 diagonals, 1 straight).
    CHECK(route_length(from, route) <= 4.0f * 1.41421356f + 4.0f + 0.001f);

    const IVec2 low{1, 6};
    REQUIRE(finder.find(g, low, {7, 6}, GridDirs::Eight, route).value() == PathStatus::Found);
    for (int i = 0; i < route.count; ++i)
        CHECK(route.cells[static_cast<std::size_t>(i)].y >= low.y);
    CHECK(route_free_of_walls(g, low, route));
}

TEST_CASE("grid path: corridors one cell wide give exactly their corners") {
    const CostGrid g =
        grid_from({"#######", "#.....#", "#.###.#", "#.#...#", "#.#.###", "#.#...#", "#######"});
    PathFinder finder = finder_for(g);
    PathRoute route;
    REQUIRE(finder.find(g, {1, 5}, {5, 5}, GridDirs::Eight, route).value() == PathStatus::Found);
    const IVec2 expected[6] = {{1, 1}, {5, 1}, {5, 3}, {3, 3}, {3, 5}, {5, 5}};
    REQUIRE(route.count == 6);
    for (int i = 0; i < 6; ++i)
        CHECK(route.cells[static_cast<std::size_t>(i)] == expected[i]);
    // All 17 free cells lie on the way (the start included): each expanded exactly once.
    CHECK(finder.last_expansions() == 17);
    CHECK(finder.searches_left() == 99);
}

TEST_CASE("grid path: of two ways the cheaper one, counted 10 straight and 14 diagonal") {
    const CostGrid g =
        grid_from({"#######", "#.....#", "#.###.#", "#.###.#", "#.###.#", "#.....#", "#######"});
    PathFinder finder = finder_for(g);
    PathRoute route;
    REQUIRE(finder.find(g, {1, 2}, {5, 2}, GridDirs::Eight, route).value() == PathStatus::Found);
    REQUIRE(route.count == 3);
    CHECK(route.cells[0] == IVec2{1, 1});
    CHECK(route.cells[1] == IVec2{5, 1});
    REQUIRE(finder.find(g, {1, 4}, {5, 4}, GridDirs::Eight, route).value() == PathStatus::Found);
    REQUIRE(route.count == 3);
    CHECK(route.cells[0] == IVec2{1, 5});
    CHECK(route.cells[1] == IVec2{5, 5});
}

TEST_CASE("grid path: no way through a one-cell diagonal gap") {
    const CostGrid g = grid_from({".#...", "#....", "....."});
    PathFinder finder = finder_for(g);
    PathRoute route;
    CHECK(finder.find(g, {0, 0}, {3, 2}, GridDirs::Eight, route).value() == PathStatus::Partial);
    CHECK_FALSE(route.complete);
    REQUIRE(route.count == 1);
    CHECK(route.cells[0] == IVec2{0, 0});
    CHECK(finder.last_expansions() == 1);

    CHECK(finder.find(g, {3, 2}, {0, 0}, GridDirs::Eight, route).value() == PathStatus::Partial);
    CHECK_FALSE(route.complete);
    REQUIRE(route.count >= 1);
    CHECK(route.cells[static_cast<std::size_t>(route.count - 1)] == IVec2{1, 1});
    CHECK(route.goal == IVec2{0, 0});
    CHECK(route_free_of_walls(g, {3, 2}, route));

    const CostGrid corner = grid_from({"#####", "#.#.#", "#...#", "#####"});
    PathFinder other = finder_for(corner);
    REQUIRE(other.find(corner, {1, 1}, {3, 1}, GridDirs::Eight, route).value() ==
            PathStatus::Found);
    REQUIRE(route.count == 3);
    CHECK(route.cells[0] == IVec2{1, 2});
    CHECK(route.cells[1] == IVec2{3, 2});
    CHECK(route.cells[2] == IVec2{3, 1});
}

TEST_CASE("grid path: a goal that cannot be reached ends the search, with the nearest cell") {
    const CostGrid g =
        grid_from({"..........", "......###.", "......#.#.", "......###.", ".........."});
    const int free_outside = 50 - 8 - 1;
    PathFinder finder = finder_for(g);
    PathRoute route;

    SUBCASE("walled in") {
        CHECK(finder.find(g, {0, 2}, {7, 2}, GridDirs::Eight, route).value() ==
              PathStatus::Partial);
        CHECK_FALSE(route.complete);
        CHECK(finder.last_expansions() == free_outside);
        REQUIRE(route.count >= 1);
        // (7, 0), (5, 2), (9, 2) and (7, 4) are two straight steps from the goal; the lowest index
        // wins.
        CHECK(route.cells[static_cast<std::size_t>(route.count - 1)] == IVec2{7, 0});
        CHECK(route.goal == IVec2{7, 2});
        CHECK(route_free_of_walls(g, {0, 2}, route));
    }
    SUBCASE("inside a wall") {
        CHECK(finder.find(g, {0, 2}, {6, 2}, GridDirs::Eight, route).value() ==
              PathStatus::Partial);
        CHECK(finder.last_expansions() == free_outside);
        REQUIRE(route.count >= 1);
        CHECK(route.cells[static_cast<std::size_t>(route.count - 1)] == IVec2{5, 2});
    }
    SUBCASE("outside the grid, however far") {
        const IVec2 far{1000000, 2};
        CHECK(finder.find(g, {0, 2}, far, GridDirs::Eight, route).value() == PathStatus::Partial);
        CHECK(finder.last_expansions() == free_outside);
        REQUIRE(route.count >= 1);
        CHECK(route.cells[static_cast<std::size_t>(route.count - 1)].x == 9);
        CHECK_FALSE(route_stale(route, far, 0, 0, 20));
    }
    SUBCASE("from inside the room: the walker stays on its cell") {
        CHECK(finder.find(g, {7, 2}, {0, 0}, GridDirs::Eight, route).value() ==
              PathStatus::Partial);
        CHECK(finder.last_expansions() == 1);
        REQUIRE(route.count == 1);
        CHECK(route.cells[0] == IVec2{7, 2});
    }
    SUBCASE("questions that cannot be asked give an empty route or an error") {
        route.count = 5;
        route.complete = true;
        CHECK(finder.find(g, {6, 1}, {0, 0}, GridDirs::Eight, route).value() ==
              PathStatus::Partial); // start in a wall
        CHECK(route.count == 0);
        CHECK_FALSE(route.complete);
        route.count = 5;
        CHECK(finder.find(g, {-1, 0}, {0, 0}, GridDirs::Eight, route).value() ==
              PathStatus::Partial); // outside
        CHECK(route.count == 0);
        CHECK(finder.searches_left() == 100);
        // A grid larger than the storage is an error now, not a silent empty route.
        PathFinder tiny;
        tiny.resize(49);
        tiny.begin_tick(5);
        const Result<PathStatus> big = tiny.find(g, {0, 2}, {9, 4}, GridDirs::Eight, route);
        REQUIRE_FALSE(big.ok());
        CHECK(big.error().code == ErrorCode::OutOfRange);
        CHECK(tiny.searches_left() == 5);
        // A finder that was never sized: an error.
        PathFinder none;
        none.begin_tick(5);
        const Result<PathStatus> unsized = none.find(g, {0, 2}, {9, 4}, GridDirs::Eight, route);
        REQUIRE_FALSE(unsized.ok());
        CHECK(unsized.error().code == ErrorCode::Invalid);
        CHECK(unsized.error().message.find("never sized") != std::string::npos);
    }
}

TEST_CASE("grid path: the budget of a tick is respected and the old route is kept") {
    const CostGrid g = grid_from({".....", ".###.", "....."});
    PathFinder finder;
    finder.resize(15);
    finder.begin_tick(2);
    PathRoute first;
    PathRoute second;
    CHECK(finder.find(g, {2, 0}, {2, 2}, GridDirs::Eight, first).value() == PathStatus::Found);
    CHECK(finder.searches_left() == 1);
    CHECK(finder.find(g, {2, 2}, {2, 0}, GridDirs::Eight, second).value() == PathStatus::Found);
    CHECK(finder.searches_left() == 0);

    PathRoute third = first;
    third.next = 1;
    third.computed_tick = 7;
    CHECK(finder.find(g, {1, 0}, {2, 2}, GridDirs::Eight, third).value() == PathStatus::NoBudget);
    CHECK(third.count == first.count);
    CHECK(third.next == 1);
    CHECK(third.computed_tick == 7);
    CHECK(third.complete);
    CHECK(third.cells[0] == first.cells[0]);
    CHECK(finder.searches_left() == 0);

    PathRoute plain; // what needs no search is still answered
    CHECK(finder.find(g, {0, 0}, {4, 0}, GridDirs::Eight, plain).value() == PathStatus::Found);
    CHECK(plain.count == 1);

    finder.begin_tick(2);
    CHECK(finder.find(g, {1, 0}, {2, 2}, GridDirs::Eight, third).value() == PathStatus::Found);
    CHECK(third.next == 0);
    CHECK(finder.searches_left() == 1);
    finder.begin_tick(0);
    CHECK(finder.find(g, {1, 0}, {2, 2}, GridDirs::Eight, third).value() == PathStatus::NoBudget);
}

TEST_CASE("grid path: a pack asking on the same tick is spread over ticks, two at a time") {
    const CostGrid g = grid_from({".......", ".#####.", "......."});
    PathFinder finder;
    finder.resize(21);
    const IVec2 goal{3, 2};
    PathRoute routes[5];
    int served = 0;
    for (int tick = 0; tick < 3; ++tick) {
        finder.begin_tick(2);
        int this_tick = 0;
        for (int i = 0; i < 5; ++i) {
            if (!route_stale(routes[i], goal, static_cast<std::uint64_t>(tick),
                             routes[i].computed_tick, 20))
                continue;
            if (finder.find(g, {1 + i, 0}, goal, GridDirs::Eight, routes[i]).value() ==
                PathStatus::Found) {
                routes[i].computed_tick = static_cast<std::uint64_t>(tick);
                ++this_tick;
            }
        }
        CHECK(this_tick <= 2);
        served += this_tick;
    }
    CHECK(served == 5);
}

TEST_CASE("grid path: a route is reused until the goal changes cell, runs out or grows old") {
    const CostGrid g = grid_from({".....", ".###.", "....."});
    PathFinder finder = finder_for(g);
    PathRoute route;
    CHECK(route_stale(route, {2, 2}, 0, 0, 20)); // none yet

    REQUIRE(finder.find(g, {2, 0}, {2, 2}, GridDirs::Eight, route).value() == PathStatus::Found);
    route.computed_tick = 100;
    CHECK(finder.searches_left() == 99);
    CHECK_FALSE(route_stale(route, {2, 2}, 100, route.computed_tick, 20));
    CHECK(
        route_stale(route, {3, 2}, 100, route.computed_tick, 20)); // the goal moved to another cell
    CHECK(route_stale(route, {2, 3}, 100, route.computed_tick, 20));
    // Age: at repath_ticks the route is old.
    CHECK_FALSE(route_stale(route, {2, 2}, 119, route.computed_tick, 20));
    CHECK(route_stale(route, {2, 2}, 120, route.computed_tick, 20));
    CHECK_FALSE(route_stale(route, {2, 2}, 120, route.computed_tick, 21));
    // Computed "after now" (an undo went back in time): stale.
    CHECK(route_stale(route, {2, 2}, 99, route.computed_tick, 20));

    REQUIRE(finder.find(g, {2, 0}, {3, 2}, GridDirs::Eight, route).value() == PathStatus::Found);
    CHECK(route.goal == IVec2{3, 2});
    CHECK_FALSE(route_stale(route, {3, 2}, 100, 100, 20));
    CHECK(route_stale(route, {2, 2}, 100, 100, 20));

    // Used up: a complete route stays good (the walker has arrived), a cut one does not.
    route.next = route.count;
    CHECK_FALSE(route_stale(route, {3, 2}, 100, 100, 20));
    route.complete = false;
    CHECK(route_stale(route, {3, 2}, 100, 100, 20));
    route.next = 0;
    CHECK_FALSE(route_stale(route, {3, 2}, 100, 100, 20));
}

TEST_CASE("grid path: following a route gives a unit direction and steps past reached points") {
    TileWorld tw;
    tw.resize(8, 8, 16, {0, 0});
    PathRoute route;
    CHECK(follow_route(route, tw, {5, 5}) == Vec2{0, 0});

    route.cells[0] = {0, 0}; // centre (8, 8)
    route.cells[1] = {1, 0}; // centre (24, 8)
    route.cells[2] = {1, 3}; // centre (24, 56)
    route.count = 3;
    route.complete = true;

    Vec2 dir = follow_route(route, tw, {0, 8});
    CHECK(route.next == 0);
    CHECK(dir.x == doctest::Approx(1.0f));
    CHECK(dir.y == doctest::Approx(0.0f));

    // Within the arrive radius of the first point and so also of the second: both are done.
    dir = follow_route(route, tw, {16, 8}, 8.0f);
    CHECK(route.next == 2);
    CHECK(dir.x == doctest::Approx(8.0f / std::hypot(8.0f, 48.0f)));
    CHECK(dir.y == doctest::Approx(48.0f / std::hypot(8.0f, 48.0f)));
    CHECK(length(dir) == doctest::Approx(1.0f));

    // Exactly on the radius counts as there.
    dir = follow_route(route, tw, {24, 54}, 2.0f);
    CHECK(route.next == 3);
    CHECK(dir == Vec2{0, 0});
    CHECK(follow_route(route, tw, {0, 0}) == Vec2{0, 0});
    CHECK(route.next == 3);
}

TEST_CASE("grid path: smoothing never takes a route across a wall") {
    const CostGrid g =
        grid_from({"................", "..##.....#......", "..#......#..##..", "..#..###.#...#..",
                   ".....#.......#..", ".###.#..####.#..", "...#.#.....#....", "...#....##.#.##.",
                   ".........#......", "................"});
    for (const PathSmoothing smoothing : {PathSmoothing::LineOfSight, PathSmoothing::Corners}) {
        PathFinder finder = finder_for(g, 1000000);
        finder.set_smoothing(smoothing);
        PathRoute route;
        int found = 0;
        int smoothed = 0;
        for (int a = 0; a < 160; a += 3) {
            if (g.cells()[static_cast<std::size_t>(a)] == 0) continue;
            for (int b = 0; b < 160; ++b) {
                if (g.cells()[static_cast<std::size_t>(b)] == 0) continue;
                const IVec2 from{a % 16, a / 16};
                const IVec2 goal{b % 16, b / 16};
                const Result<PathStatus> status =
                    finder.find(g, from, goal, GridDirs::Eight, route);
                REQUIRE(status.value() == PathStatus::Found); // everything free here is connected
                REQUIRE(route.count >= 1);
                CHECK(route.cells[static_cast<std::size_t>(route.count - 1)] == goal);
                const bool free = route_free_of_walls(g, from, route);
                CHECK_MESSAGE(free, "from cell " << a << " to cell " << b);
                if (!free) return;
                ++found;
                if (finder.last_expansions() > route.count + 1) ++smoothed;
            }
        }
        CHECK(found > 5000);
        CHECK(smoothed > 1000);
    }
}

TEST_CASE("grid path: the same question always gives the same route") {
    const CostGrid g = grid_from({".........", ".##.###..", ".#.....#.", ".#.###.#.", "...#.#...",
                                  ".#.....#.", "........."});
    PathFinder one = finder_for(g);
    PathFinder two = finder_for(g);
    PathRoute first;
    PathRoute again;
    PathRoute fresh;
    REQUIRE(one.find(g, {0, 0}, {4, 4}, GridDirs::Eight, first).value() == PathStatus::Found);
    const int expansions = one.last_expansions();
    REQUIRE(one.find(g, {8, 6}, {2, 2}, GridDirs::Eight, again).value() == PathStatus::Found);
    REQUIRE(one.find(g, {0, 0}, {4, 4}, GridDirs::Eight, again).value() == PathStatus::Found);
    REQUIRE(two.find(g, {0, 0}, {4, 4}, GridDirs::Eight, fresh).value() == PathStatus::Found);
    CHECK(one.last_expansions() == expansions);
    CHECK(two.last_expansions() == expansions);
    REQUIRE(again.count == first.count);
    REQUIRE(fresh.count == first.count);
    for (int i = 0; i < first.count; ++i) {
        CHECK(again.cells[static_cast<std::size_t>(i)] == first.cells[static_cast<std::size_t>(i)]);
        CHECK(fresh.cells[static_cast<std::size_t>(i)] == first.cells[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("grid path: a route with too many corners is cut and walked in pieces") {
    CostGrid g = open_grid(41, 9);
    for (int x = 1; x < 41; x += 2) {
        for (int y = 0; y < 9; ++y) {
            const bool gap = ((x / 2) % 2 == 0) ? y == 8 : y == 0;
            if (!gap) g(x, y) = 0;
        }
    }
    PathFinder finder = finder_for(g);
    PathRoute route;
    CHECK(finder.find(g, {0, 0}, {40, 0}, GridDirs::Eight, route).value() == PathStatus::Partial);
    CHECK(route.count == kMaxRoutePoints);
    CHECK_FALSE(route.complete);
    CHECK(route.goal == IVec2{40, 0});
    CHECK(route_free_of_walls(g, {0, 0}, route));
    route.next = route.count; // used up and not complete: ask again from where it stands
    CHECK(route_stale(route, {40, 0}, 0, 0, 1000));
    CHECK(walk_to(g, mid(0, 0), {40, 0}, 20000, {0.375f, 0.375f}) > 0);
}

TEST_CASE("grid path: a walker following routes arrives, round corners, without a wall hit") {
    const CostGrid g =
        grid_from({"............", ".####.#####.", ".#........#.", ".#.######.#.", ".#.#....#.#.",
                   ".#.#.##.#.#.", ".#...##...#.", ".##########.", "............"});
    for (const Vec2 half : {Vec2{0.375f, 0.1875f}, Vec2{0.4375f, 0.3125f}}) {
        const int ticks = walk_to(g, mid(0, 8), {4, 4}, 4000, half);
        CHECK(ticks > 0);
        CHECK(ticks < 1200); // about 30 cells at 0.1 cell a tick; near 4000 would mean stuck
    }
}

TEST_CASE("grid path: the worst search on a 56 x 32 map is bounded") {
    constexpr int kW = 56;
    constexpr int kH = 32;
    CostGrid g = open_grid(kW, kH);
    for (int i = 0; i < 3; ++i) {
        g(kW - 3, kH - 1 - i) = 0;
        g(kW - 1 - i, kH - 3) = 0;
    }
    PathFinder finder;
    finder.resize(kW * kH);
    finder.begin_tick(1);
    PathRoute route;
    CHECK(finder.find(g, {0, 0}, {kW - 1, kH - 1}, GridDirs::Eight, route).value() ==
          PathStatus::Partial);
    CHECK(finder.last_expansions() == kW * kH - 5 - 4);

    CostGrid maze = open_grid(kW, kH);
    for (int x = 1; x < kW; x += 2) {
        for (int y = 0; y < kH; ++y) {
            const bool gap = ((x / 2) % 2 == 0) ? y == kH - 1 : y == 0;
            if (!gap) maze(x, y) = 0;
        }
    }
    finder.begin_tick(1);
    CHECK(finder.find(maze, {0, 0}, {kW - 2, kH - 1}, GridDirs::Eight, route).value() ==
          PathStatus::Partial);
    CHECK(route.count == kMaxRoutePoints);
    CHECK(finder.last_expansions() <= kW * kH);
    CHECK(finder.last_expansions() >= (kW / 2) * (kH - 1));
}

// ---- directions, weights and optimality ---------------------------------------------------------

TEST_CASE("grid path: four directions walk the corridors with axis-aligned legs only") {
    const CostGrid g = grid_from({"........", ".######.", "........", "######..", "........"});
    PathFinder finder = finder_for(g);
    PathRoute route;
    REQUIRE(finder.find(g, {0, 0}, {0, 4}, GridDirs::Four, route).value() == PathStatus::Found);
    IVec2 at{0, 0};
    for (int i = 0; i < route.count; ++i) {
        const IVec2 c = route.cells[static_cast<std::size_t>(i)];
        CHECK((c.x == at.x || c.y == at.y)); // one axis per leg
        at = c;
    }
    CHECK(at == IVec2{0, 4});
    // Diagonal neighbours are not reachable in one step: a pocket open only diagonally is closed.
    const CostGrid pocket = grid_from({".#", "#."});
    PathFinder p = finder_for(pocket);
    CHECK(p.find(pocket, {0, 0}, {1, 1}, GridDirs::Four, route).value() == PathStatus::Partial);
    // Manhattan heuristic: a straight run expands only the run.
    const CostGrid line = open_grid(10, 1);
    PathFinder l = finder_for(line);
    REQUIRE(l.find(line, {0, 0}, {9, 0}, GridDirs::Four, route).value() == PathStatus::Found);
    CHECK(l.last_expansions() == 10);
    CHECK(route.count == 1);
}

TEST_CASE("grid path: weighted cells are avoided when a detour is cheaper, crossed when not") {
    // A swamp band (cost 40) with a dry gap at the far end.
    const CostGrid g = grid_from({"..........", "~~~~~~~~~.", "..........", ".........."});
    PathFinder finder = finder_for(g);
    finder.set_smoothing(PathSmoothing::Corners);
    PathRoute route;
    REQUIRE(finder.find(g, {0, 0}, {0, 3}, GridDirs::Eight, route).value() == PathStatus::Found);
    // Through the swamp: 3 straight steps, one of them 40 = 60; the detour costs far more.
    IVec2 at{0, 0};
    std::uint32_t cost = 0;
    for (int i = 0; i < route.count; ++i) {
        const IVec2 c = route.cells[static_cast<std::size_t>(i)];
        while (at != c) {
            const IVec2 next{at.x + (c.x > at.x) - (c.x < at.x),
                             at.y + (c.y > at.y) - (c.y < at.y)};
            cost += step_cost(g, at, next);
            at = next;
        }
    }
    CHECK(cost == 60);
    // Make the swamp dearer than the detour: the route goes round through the gap.
    const CostGrid deep = grid_from({"..........", "#########.", "..........", ".........."});
    CostGrid dear = deep;
    for (int x = 0; x < 9; ++x)
        dear(x, 1) = 900;
    PathFinder f2 = finder_for(dear);
    f2.set_smoothing(PathSmoothing::Corners);
    REQUIRE(f2.find(dear, {0, 0}, {0, 3}, GridDirs::Eight, route).value() == PathStatus::Found);
    bool via_gap = false;
    for (int i = 0; i < route.count; ++i)
        via_gap = via_gap || route.cells[static_cast<std::size_t>(i)].x >= 8;
    CHECK(via_gap);
    // Line-of-sight smoothing never pulls a leg across costlier cells than the path it replaces.
    PathFinder f3 = finder_for(g);
    REQUIRE(f3.find(g, {0, 2}, {9, 2}, GridDirs::Eight, route).value() == PathStatus::Found);
    CHECK(route.count == 1); // dry row: a straight line
    REQUIRE(f3.find(g, {0, 0}, {9, 3}, GridDirs::Eight, route).value() == PathStatus::Found);
    CHECK(route_free_of_walls(g, {0, 0}, route));
}

TEST_CASE("grid path: A* costs equal Dijkstra map distances on random weighted grids") {
    Rng rng(1234);
    PathFinder finder;
    finder.resize(24 * 24);
    finder.set_smoothing(PathSmoothing::Corners);
    DijkstraMap dm;
    dm.resize({24, 24});
    int compared = 0;
    for (int trial = 0; trial < 60; ++trial) {
        CostGrid g(24, 24, 10);
        for (std::uint16_t &c : g.cells()) {
            const std::uint32_t r = rng.below(100);
            c = r < 22 ? 0 : (r < 40 ? static_cast<std::uint16_t>(10 + rng.below(50)) : 10);
        }
        const GridDirs dirs = trial % 2 == 0 ? GridDirs::Eight : GridDirs::Four;
        const IVec2 from{static_cast<int>(rng.below(24)), static_cast<int>(rng.below(24))};
        const IVec2 goal{static_cast<int>(rng.below(24)), static_cast<int>(rng.below(24))};
        if (g(from.x, from.y) == 0 || g(goal.x, goal.y) == 0 || from == goal) continue;
        const IVec2 goals[] = {goal};
        dm.compute(g, goals, dirs);
        finder.begin_tick(1);
        PathRoute route;
        const PathStatus s = finder.find(g, from, goal, dirs, route).value();
        if (dm.at(from) == UINT32_MAX) {
            CHECK(s == PathStatus::Partial);
            continue;
        }
        if (!route.complete) continue; // cut at kMaxRoutePoints
        IVec2 at = from;
        std::uint32_t cost = 0;
        for (int i = 0; i < route.count; ++i) {
            const IVec2 c = route.cells[static_cast<std::size_t>(i)];
            while (at != c) {
                const IVec2 next{at.x + (c.x > at.x) - (c.x < at.x),
                                 at.y + (c.y > at.y) - (c.y < at.y)};
                REQUIRE(g(next.x, next.y) != 0);
                cost += step_cost(g, at, next);
                at = next;
            }
        }
        CHECK(cost == dm.at(from));
        ++compared;
    }
    CHECK(compared > 20);
}

TEST_CASE("grid path: no allocation after resize, and the 64 x 64 budget") {
    Rng rng(99);
    CostGrid g(64, 64, 10);
    for (std::uint16_t &c : g.cells())
        c = rng.below(100) < 20 ? 0 : 10;
    g(0, 0) = 10;
    g(63, 63) = 10;
    CostGrid worst = open_grid(64, 64); // a walled-in goal: every cell expanded
    worst(62, 63) = 0;
    worst(62, 62) = 0;
    worst(63, 62) = 0;
    PathFinder finder;
    finder.resize(64 * 64);
    PathRoute route;
    finder.begin_tick(1);
    REQUIRE(finder.find(g, {0, 0}, {63, 63}, GridDirs::Eight, route).ok()); // warm-up
    CHECK_NO_ALLOC({
        for (int i = 0; i < 50; ++i) {
            finder.begin_tick(2);
            (void)finder.find(g, {0, 0}, {63, 63}, GridDirs::Eight, route);
            (void)finder.find(worst, {0, 0}, {63, 63}, GridDirs::Eight, route);
        }
    });
    // The fastest of many runs: the machine running the tests may be busy, and the budget is about
    // the search, not about the scheduler.
    const auto best_ms = [&](const CostGrid &grid) {
        double best = 1e9;
        for (int i = 0; i < 101; ++i) {
            finder.begin_tick(1);
            const auto t0 = std::chrono::steady_clock::now();
            (void)finder.find(grid, {0, 0}, {63, 63}, GridDirs::Eight, route);
            const auto t1 = std::chrono::steady_clock::now();
            best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    const double typical = best_ms(g);
    const int typical_expansions = finder.last_expansions();
    const double full = best_ms(worst);
    MESSAGE("A* 64x64: typical " << typical << " ms (" << typical_expansions
                                 << " expansions); unreachable goal " << full << " ms ("
                                 << finder.last_expansions() << " expansions, the whole map)");
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(typical <= 0.2);
    CHECK(full <= 0.4);    // a full flood of 4096 cells: regression guard, about 0.2 ms on an M1
#endif
}

// ---- Block line-of-sight smoothing --------------------------------------------------------------

namespace {

// The serpentine benchmark map: walls at x = 2, 6, ..., 62 with a one-cell gap at
// the bottom and the top in turn; corner to corner it is the longest route a 64 x 64 grid has.
CostGrid serpentine_64() {
    CostGrid g = open_grid(64, 64);
    int wall = 0;
    for (int x = 2; x < 64; x += 4, ++wall) {
        for (int y = 0; y < 64; ++y) {
            const bool gap = (wall % 2 == 0) ? y == 63 : y == 0;
            if (!gap) g(x, y) = 0;
        }
    }
    return g;
}

// String pulling as a per-cell loop does it, over the cell path rebuilt from a
// Corners route (consecutive corners lie on one of the eight step directions).
std::vector<IVec2> per_cell_smoothing(const CostGrid &g, IVec2 from, const PathRoute &corners,
                                      bool &cut) {
    std::vector<IVec2> path{from};
    for (int i = 0; i < corners.count; ++i) {
        const IVec2 c = corners.cells[static_cast<std::size_t>(i)];
        while (path.back() != c) {
            const IVec2 at = path.back();
            path.push_back(
                {at.x + (c.x > at.x) - (c.x < at.x), at.y + (c.y > at.y) - (c.y < at.y)});
        }
    }
    std::vector<IVec2> kept;
    cut = false;
    if (path.size() == 1) return {from};
    IVec2 anchor = path[0];
    std::uint16_t stretch = g(anchor.x, anchor.y);
    for (std::size_t i = 1; i < path.size(); ++i) {
        const IVec2 here = path[i];
        stretch = std::max(stretch, g(here.x, here.y));
        if (i + 1 < path.size()) {
            const IVec2 after = path[i + 1];
            const std::uint16_t limit = std::max(stretch, g(after.x, after.y));
            if (grid_box_clear(g, cell_center(anchor), cell_center(after), {0.5f, 0.5f}, limit))
                continue;
        }
        if (kept.size() == static_cast<std::size_t>(kMaxRoutePoints)) {
            cut = true;
            break;
        }
        kept.push_back(here);
        anchor = here;
        stretch = g(here.x, here.y);
    }
    return kept;
}

} // namespace

TEST_CASE("grid path: LineOfSight waypoints equal per-cell string pulling") {
    Rng rng(148);
    PathFinder los;
    PathFinder corners;
    los.resize(48 * 48);
    corners.resize(48 * 48);
    corners.set_smoothing(PathSmoothing::Corners);
    int compared = 0;
    int long_routes = 0;
    for (int map = 0; map < 160; ++map) {
        const int w = 6 + static_cast<int>(rng.below(43));
        const int h = 6 + static_cast<int>(rng.below(43));
        CostGrid g = open_grid(w, h);
        const int kind = map % 4;
        const std::uint32_t density = 3 + rng.below(25);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                std::uint16_t c = 10;
                // Scattered walls, a serpentine, rooms, sparse walls; weights on the last two.
                if (kind == 0 && rng.below(100) < density) c = 0;
                if (kind == 1 && x % 5 == 2 && !((x / 5) % 2 == 0 ? y == h - 1 : y == 0)) c = 0;
                if (kind == 2 && ((x % 7 == 3 && y % 6 != 0) || (y % 8 == 4 && x % 5 != 1))) c = 0;
                if (kind == 3 && rng.below(100) < density / 2) c = 0;
                if (c != 0 && kind >= 2 && rng.below(5) == 0)
                    c = static_cast<std::uint16_t>(5 + rng.below(40));
                g(x, y) = c;
            }
        }
        for (int q = 0; q < 40; ++q) {
            const IVec2 from{static_cast<int>(rng.below(static_cast<std::uint32_t>(w))),
                             static_cast<int>(rng.below(static_cast<std::uint32_t>(h)))};
            const IVec2 goal{static_cast<int>(rng.below(static_cast<std::uint32_t>(w))),
                             static_cast<int>(rng.below(static_cast<std::uint32_t>(h)))};
            PathRoute smooth;
            PathRoute plain;
            los.begin_tick(1);
            corners.begin_tick(1);
            const PathStatus s = los.find(g, from, goal, GridDirs::Eight, smooth).value();
            (void)corners.find(g, from, goal, GridDirs::Eight, plain).value();
            // No search (a clear straight line or a blocked start) or a cut Corners route: there
            // is no cell path to compare with.
            if (los.last_expansions() == 0 || plain.count == kMaxRoutePoints) continue;
            bool cut = false;
            const std::vector<IVec2> expected = per_cell_smoothing(g, from, plain, cut);
            REQUIRE(static_cast<int>(expected.size()) == smooth.count);
            for (int i = 0; i < smooth.count; ++i)
                CHECK(smooth.cells[static_cast<std::size_t>(i)] ==
                      expected[static_cast<std::size_t>(i)]);
            if (cut) CHECK(s == PathStatus::Partial);
            ++compared;
            if (expected.size() >= 4) ++long_routes;
        }
    }
    CHECK(compared > 2000);
    CHECK(long_routes > 300);
}

TEST_CASE("grid path: the 64 x 64 serpentine is searched and smoothed within 0.2 ms") {
    const CostGrid g = serpentine_64();
    PathFinder finder;
    finder.resize(64 * 64);
    PathRoute route;
    finder.begin_tick(1);
    REQUIRE(finder.find(g, {0, 0}, {63, 63}, GridDirs::Eight, route).value() ==
            PathStatus::Partial); // 33 waypoints: cut at kMaxRoutePoints
    CHECK(finder.last_expansions() == 3088);
    REQUIRE(route.count == kMaxRoutePoints);
    // Two waypoints per wall: the gap's near side, then the far side's corner.
    CHECK(route.cells[0] == IVec2{1, 63});
    for (int wall = 0; wall < 15; ++wall) {
        const int bottom = wall % 2 == 0 ? 63 : 0;
        CHECK(route.cells[static_cast<std::size_t>(1 + 2 * wall)] == IVec2{3 + 4 * wall, bottom});
        CHECK(route.cells[static_cast<std::size_t>(2 + 2 * wall)] ==
              IVec2{5 + 4 * wall, 63 - bottom});
    }
    CHECK(route.cells[31] == IVec2{63, 0});
    CHECK(route_free_of_walls(g, {0, 0}, route));
    CHECK_NO_ALLOC({
        for (int i = 0; i < 20; ++i) {
            finder.begin_tick(1);
            (void)finder.find(g, {0, 0}, {63, 63}, GridDirs::Eight, route);
        }
    });
    double best = 1e9;
    for (int i = 0; i < 101; ++i) {
        finder.begin_tick(1);
        const auto t0 = std::chrono::steady_clock::now();
        (void)finder.find(g, {0, 0}, {63, 63}, GridDirs::Eight, route);
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    MESSAGE("A* 64x64 serpentine with LineOfSight smoothing: " << best << " ms");
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
    CHECK(best <= 0.2); // about 0.09 ms on an M1
#endif
}

// ---- Regressions --------------------------------------------------------------------------------

TEST_CASE("grid path: a route whose count exceeds its storage is walked to its last cell only") {
    TileWorld tw;
    tw.resize(4, 4, 16, {0, 0});
    PathRoute route;
    route.count = 40; // more waypoints than PathRoute holds (game code, inspector, bad save)
    route.complete = true;
    route.goal = {3, 3};
    // Every stored waypoint is the cell we stand on, so all 32 are reached at once; nothing past
    // cells[31] may be read as a waypoint.
    const Vec2 dir = follow_route(route, tw, tw.tile_center({0, 0}));
    CHECK(route.next == kMaxRoutePoints);
    CHECK(dir == Vec2{0, 0});
}

TEST_CASE("grid path: saved routes round-trip; a count past the storage is a load error") {
    const ComponentInfo &info = ComponentRegistry::global().info<PathRoute>();
    REQUIRE(info.read != nullptr);
    REQUIRE(info.write != nullptr);
    REQUIRE(info.hash != nullptr);
    const auto read = [&](std::string_view json, PathRoute &out) {
        Result<DataDocument> doc = DataDocument::parse(json, "save.json");
        REQUIRE(doc.ok());
        DataReader root = doc->root();
        const Status s = info.read(&out, root, SerializeContext{});
        return s.ok() && doc->finish().ok();
    };
    const auto hash = [&](const PathRoute &r) {
        Hasher h;
        info.hash(&r, h);
        return h.value();
    };
    PathRoute route;
    route.cells[0] = {3, 4};
    route.cells[1] = {5, 6};
    route.cells[31] = {-7, 9};
    route.count = 2;
    route.next = 1;
    route.complete = true;
    route.goal = {5, 6};
    route.computed_tick = 77;
    DataWriter out;
    info.write(&route, out, SerializeContext{});
    const Result<std::string> json = out.finish();
    REQUIRE(json.ok());
    CHECK(json->find("\"computed_tick\"") != std::string::npos);
    PathRoute back;
    REQUIRE(read(*json, back));
    CHECK(back.cells == route.cells);
    CHECK(back.count == 2);
    CHECK(back.next == 1);
    CHECK(back.complete);
    CHECK(back.goal == IVec2{5, 6});
    CHECK(back.computed_tick == 77);
    CHECK(hash(back) == hash(route));
    back.cells[31] = {0, 0};
    CHECK(hash(back) != hash(route));
    PathRoute bad;
    CHECK_FALSE(read(R"({"count": 40, "next": 0})", bad));
    CHECK_FALSE(read(R"({"count": 33})", bad));
    CHECK_FALSE(read(R"({"count": 3, "next": 4})", bad));
    PathRoute full;
    CHECK(read(R"({"count": 32, "next": 32, "complete": true})", full));
    CHECK(full.count == 32);
}
