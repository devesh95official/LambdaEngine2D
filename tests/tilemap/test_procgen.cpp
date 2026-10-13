// Procedural generation: BSP rooms, caves and connect_regions.
#include <cstdint>
#include <vector>

#include <lam/math/rng.h>
#include <lam/test/support.h>
#include <lam/tilemap/procgen.h>

using namespace lam;

namespace {

// Number of 4-connected regions of `floor` cells.
int regions(const Grid<std::uint8_t> &g, std::uint8_t floor = 1) {
    Grid<std::uint8_t> seen(g.width(), g.height(), 0);
    std::vector<IVec2> stack;
    int n = 0;
    for (int y = 0; y < g.height(); ++y) {
        for (int x = 0; x < g.width(); ++x) {
            if (g(x, y) != floor || seen(x, y) != 0) continue;
            ++n;
            stack.push_back({x, y});
            seen(x, y) = 1;
            while (!stack.empty()) {
                const IVec2 p = stack.back();
                stack.pop_back();
                for (const IVec2 d : {IVec2{1, 0}, IVec2{-1, 0}, IVec2{0, 1}, IVec2{0, -1}}) {
                    const IVec2 q = p + d;
                    if (!g.inside(q) || g(q.x, q.y) != floor || seen(q.x, q.y) != 0) continue;
                    seen(q.x, q.y) = 1;
                    stack.push_back(q);
                }
            }
        }
    }
    return n;
}

bool border_is_wall(const Grid<std::uint8_t> &g) {
    for (int x = 0; x < g.width(); ++x) {
        if (g(x, 0) != 0 || g(x, g.height() - 1) != 0) return false;
    }
    for (int y = 0; y < g.height(); ++y) {
        if (g(0, y) != 0 || g(g.width() - 1, y) != 0) return false;
    }
    return true;
}

} // namespace

TEST_CASE("procgen: BSP rooms fit their bounds, are carved and are all connected") {
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        Grid<std::uint8_t> g(64, 40, 7);
        Rng rng(seed);
        std::vector<IRect> rooms;
        BspParams p;
        p.min_room = 4;
        p.max_room = 9;
        p.depth = 4;
        bsp_rooms(g, rng, p, rooms);
        INFO("seed " << seed);
        REQUIRE(rooms.size() >= 4);
        CHECK(rooms.size() <= 16);
        CHECK(border_is_wall(g));
        for (const IRect &r : rooms) {
            CHECK(r.w >= 4);
            CHECK(r.w <= 9);
            CHECK(r.h >= 4);
            CHECK(r.h <= 9);
            CHECK(r.x >= 1);
            CHECK(r.y >= 1);
            CHECK(r.x + r.w <= 63);
            CHECK(r.y + r.h <= 39);
            for (int y = r.y; y < r.y + r.h; ++y) {
                for (int x = r.x; x < r.x + r.w; ++x)
                    REQUIRE(g(x, y) == 1);
            }
        }
        CHECK(regions(g) == 1);
        for (const std::uint8_t c : g.cells())
            REQUIRE(c <= 1); // the initial fill (7) is overwritten
    }
}

TEST_CASE("procgen: the same seed gives the same level, another seed another") {
    const auto level = [](std::uint64_t seed) {
        Grid<std::uint8_t> g(48, 32);
        Rng rng(seed);
        std::vector<IRect> rooms;
        bsp_rooms(g, rng, {}, rooms);
        return g;
    };
    CHECK(level(5) == level(5));
    CHECK_FALSE(level(5) == level(6));
    const auto cave = [](std::uint64_t seed) {
        Grid<std::uint8_t> g(48, 32);
        Rng rng(seed);
        cellular_caves(g, rng, 0.45f, 4);
        connect_regions(g, rng);
        return g;
    };
    CHECK(cave(9) == cave(9));
    CHECK_FALSE(cave(9) == cave(10));
}

TEST_CASE("procgen: caves keep a wall border; connect_regions joins every region") {
    int joined_total = 0;
    for (std::uint64_t seed = 1; seed <= 15; ++seed) {
        Grid<std::uint8_t> g(50, 30);
        Rng rng(seed);
        cellular_caves(g, rng, 0.47f, 4);
        CHECK(border_is_wall(g));
        const int before = regions(g);
        const int joined = connect_regions(g, rng);
        INFO("seed " << seed);
        CHECK(joined == (before > 1 ? before - 1 : 0));
        CHECK(regions(g) <= 1);
        joined_total += joined;
    }
    CHECK(joined_total > 0); // some caves were split
    // Extremes: no walls inside, or all walls.
    Grid<std::uint8_t> open(10, 8);
    Rng rng(3);
    cellular_caves(open, rng, 0.0f, 3);
    CHECK(border_is_wall(open));
    CHECK(open(5, 4) == 1);
    CHECK(regions(open) == 1);
    Grid<std::uint8_t> solid(10, 8);
    cellular_caves(solid, rng, 1.0f, 3);
    CHECK(regions(solid) == 0);
    CHECK(connect_regions(solid, rng) == 0);
    // A custom floor value.
    Grid<std::uint8_t> two(9, 3, 0);
    two(1, 1) = 5;
    two(7, 1) = 5;
    CHECK(connect_regions(two, rng, 5) == 1);
    CHECK(regions(two, 5) == 1);
}

TEST_CASE("procgen: grids too small for a room stay walls") {
    Grid<std::uint8_t> tiny(2, 2, 1);
    Rng rng(1);
    std::vector<IRect> rooms{{1, 1, 1, 1}};
    bsp_rooms(tiny, rng, {}, rooms);
    CHECK(rooms.empty());
    CHECK(tiny == Grid<std::uint8_t>(2, 2, 0));
    Grid<std::uint8_t> narrow(30, 5, 1);
    bsp_rooms(narrow, rng, {}, rooms); // interior 3 high: below min_room
    CHECK(rooms.empty());
    CHECK(regions(narrow) == 0);
}
