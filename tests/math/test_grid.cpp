// Grid<T>: sizes, bounds-safe access and row-major indexing.
#include <doctest/doctest.h>

#include <cstdint>

#include <lam/math/grid.h>

using namespace lam;

TEST_CASE("grid: negative sizes clamp to zero") {
    Grid<int> g(-3, 5);
    CHECK(g.width() == 0);
    CHECK(g.height() == 5);
    CHECK(g.size() == 0);
    CHECK_FALSE(g.inside(0, 0));
    CHECK(g.get_or(0, 0, -1) == -1);
    g.set(0, 0, 9); // ignored, no crash
    CHECK(g.size() == 0);
    g.resize(-1, -1);
    CHECK(g.width() == 0);
    CHECK(g.height() == 0);
    CHECK(g.cells().empty());
    CHECK(g.coord(5) == IVec2{});
    Grid<int> empty;
    CHECK(empty.size() == 0);
    CHECK(empty.get_or({0, 0}, 4) == 4);
}

TEST_CASE("grid: get_or returns the fallback outside; set outside is ignored") {
    Grid<std::uint8_t> g(4, 3, 1);
    CHECK(g.size() == 12);
    CHECK(g.get_or(0, 0, 0) == 1);
    CHECK(g.get_or(3, 2, 0) == 1);
    CHECK(g.get_or(4, 0, 7) == 7);
    CHECK(g.get_or(0, 3, 7) == 7);
    CHECK(g.get_or(-1, 0, 7) == 7);
    CHECK(g.get_or(IVec2{0, -1}, 7) == 7);
    const Grid<std::uint8_t> before = g;
    g.set(-1, 0, 9);
    g.set(4, 0, 9);
    g.set(0, 3, 9);
    g.set(0, -1, 9);
    CHECK(g == before);
    g.set(2, 1, 9);
    CHECK(g(2, 1) == 9);
    CHECK(g.get_or(2, 1, 0) == 9);
    CHECK_FALSE(g == before);
}

TEST_CASE("grid: row-major index and coord round-trip") {
    Grid<int> g(5, 4);
    CHECK(g.index(0, 0) == 0);
    CHECK(g.index(4, 0) == 4);
    CHECK(g.index(0, 1) == 5);
    for (int y = 0; y < g.height(); ++y) {
        for (int x = 0; x < g.width(); ++x) {
            REQUIRE(g.coord(g.index(x, y)) == IVec2{x, y});
            g(x, y) = y * 10 + x;
        }
    }
    CHECK(g.cells()[7] == 12); // (2, 1)
    CHECK(g.inside(IVec2{4, 3}));
    CHECK_FALSE(g.inside(IVec2{5, 3}));
    g.fill(3);
    for (int v : g.cells())
        REQUIRE(v == 3);
    const Grid<int> &cg = g;
    CHECK(cg(1, 1) == 3);
    CHECK(cg.cells().size() == 20);
    g.resize(2, 2, 8);
    CHECK(g.size() == 4);
    CHECK(g(1, 1) == 8);
}
