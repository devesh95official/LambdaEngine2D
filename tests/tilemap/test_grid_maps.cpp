// Dijkstra maps and flow fields, using PathFinder's step costs.
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <lam/math/rng.h>
#include <lam/physics/tile_world.h>
#include <lam/test/support.h>
#include <lam/tilemap/grid_maps.h>

using namespace lam;

namespace {

constexpr std::uint32_t kFar = std::numeric_limits<std::uint32_t>::max();

// '#' blocked, '~' cost 40, else 10.
Grid<std::uint16_t> cost_from(std::initializer_list<const char *> rows) {
    const int w = static_cast<int>(std::string_view(*rows.begin()).size());
    Grid<std::uint16_t> g(w, static_cast<int>(rows.size()), 10);
    int y = 0;
    for (const char *row : rows) {
        for (int x = 0; x < w; ++x) {
            if (row[x] == '#') g(x, y) = 0;
            if (row[x] == '~') g(x, y) = 40;
        }
        ++y;
    }
    return g;
}

DijkstraMap map_for(const Grid<std::uint16_t> &cost, std::initializer_list<IVec2> goals,
                    GridDirs dirs = GridDirs::Eight) {
    DijkstraMap d;
    d.resize({cost.width(), cost.height()});
    const std::vector<IVec2> g(goals);
    d.compute(cost, g, dirs);
    return d;
}

} // namespace

TEST_CASE("dijkstra map: distances use 10 straight, 14 diagonal and the entered cell's cost") {
    const Grid<std::uint16_t> open(7, 5, 10);
    const DijkstraMap d = map_for(open, {{3, 2}});
    CHECK(d.size() == IVec2{7, 5});
    CHECK(d.dirs() == GridDirs::Eight);
    CHECK(d.at({3, 2}) == 0);
    CHECK(d.at({4, 2}) == 10);
    CHECK(d.at({4, 3}) == 14);
    CHECK(d.at({6, 2}) == 30);
    CHECK(d.at({6, 4}) == 38); // two diagonals and a straight step
    CHECK(d.at({-1, 0}) == kFar);
    CHECK(d.at({7, 0}) == kFar);
    // Four directions: Manhattan.
    const DijkstraMap four = map_for(open, {{3, 2}}, GridDirs::Four);
    CHECK(four.at({4, 3}) == 20);
    CHECK(four.at({6, 4}) == 50);
    // Entering a swamp costs 40; leaving it costs the cell entered.
    const Grid<std::uint16_t> swamp = cost_from({"..~.."});
    const DijkstraMap s = map_for(swamp, {{0, 0}});
    CHECK(s.at({1, 0}) == 10); // to reach the goal from (1, 0) it enters (0, 0)
    CHECK(s.at({2, 0}) == 20);
    CHECK(s.at({3, 0}) == 60); // enters the swamp on the way
    CHECK(s.at({4, 0}) == 70);
}

TEST_CASE("dijkstra map: walls block, corners are not cut, islands are unreachable") {
    const Grid<std::uint16_t> cost = cost_from({
        ".#...",
        "#..#.",
        "...#.",
        "####.",
        "..#..",
    });
    const DijkstraMap d = map_for(cost, {{4, 4}});
    CHECK(d.at({1, 0}) == kFar); // a wall
    CHECK(d.at({0, 0}) == kFar); // walled in diagonally: (1, 0) and (0, 1) block the corner
    CHECK(d.at({0, 4}) == kFar); // behind the wall row
    CHECK(d.at({4, 0}) == 40);
    CHECK(d.at({3, 4}) == 10);
    CHECK(d.at({2, 2}) != kFar);
    // A goal on a wall, outside the grid or twice is fine.
    const DijkstraMap odd = map_for(cost, {{1, 0}, {-3, 2}, {4, 4}, {4, 4}});
    CHECK(odd.at({4, 4}) == 0);
    CHECK(odd.at({1, 0}) == kFar);
    const DijkstraMap none = map_for(cost, {{1, 0}});
    CHECK(none.at({4, 4}) == kFar);
    CHECK_FALSE(none.step_toward_goal({4, 4}).has_value());
}

TEST_CASE("dijkstra map: the nearest of several goals wins, and walking downhill reaches it") {
    Rng rng(21);
    Grid<std::uint16_t> cost(30, 20, 10);
    for (std::uint16_t &c : cost.cells()) {
        const std::uint32_t r = rng.below(100);
        c = r < 22 ? 0 : (r < 30 ? 35 : 10);
    }
    const std::vector<IVec2> goals = {{2, 2}, {27, 17}, {15, 10}};
    for (const IVec2 g : goals)
        cost(g.x, g.y) = 10;
    DijkstraMap d;
    d.resize({30, 20});
    d.compute(cost, goals, GridDirs::Eight);
    DijkstraMap single[3];
    for (int i = 0; i < 3; ++i) {
        single[i].resize({30, 20});
        const IVec2 one[] = {goals[static_cast<std::size_t>(i)]};
        single[i].compute(cost, one, GridDirs::Eight);
    }
    int walked = 0;
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 30; ++x) {
            const std::uint32_t best =
                std::min({single[0].at({x, y}), single[1].at({x, y}), single[2].at({x, y})});
            REQUIRE(d.at({x, y}) == best);
            if (best == kFar || best == 0) continue;
            // Downhill steps add up exactly to the distance and end on a goal.
            IVec2 p{x, y};
            std::uint32_t paid = 0;
            for (int guard = 0; guard < 600; ++guard) {
                const std::optional<IVec2> next = d.step_toward_goal(p);
                if (!next) break;
                const bool diag = next->x != p.x && next->y != p.y;
                const std::uint32_t c = cost(next->x, next->y);
                paid += diag ? (c * 14u + 5u) / 10u : c;
                REQUIRE(d.at(*next) < d.at(p));
                p = *next;
            }
            CHECK(d.at(p) == 0);
            CHECK(paid == best);
            ++walked;
        }
    }
    CHECK(walked > 200);
}

TEST_CASE("dijkstra map: downhill ties go to the neighbour settled first") {
    const Grid<std::uint16_t> open(5, 5, 10);
    const DijkstraMap d = map_for(open, {{0, 2}, {4, 2}});
    // (2, 2) is 20 from both goals; (1, 2) and (3, 2) tie at 10, and (1, 2) has the lower index.
    CHECK(d.downhill({2, 2}) == IVec2{1, 2});
    CHECK(d.step_toward_goal({2, 2}) == IVec2{1, 2});
    CHECK(d.downhill({0, 2}) == IVec2{0, 2}); // a goal stays
    CHECK(d.downhill({9, 9}) == IVec2{9, 9}); // outside stays
    // The same compute twice gives the same steps everywhere.
    const DijkstraMap again = map_for(open, {{0, 2}, {4, 2}});
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 5; ++x)
            CHECK(again.downhill({x, y}) == d.downhill({x, y}));
    }
}

TEST_CASE("flow field: unit directions toward the next cell centre, zero at goals and outside") {
    const Grid<std::uint16_t> cost = cost_from({
        ".....",
        ".###.",
        ".....",
    });
    const DijkstraMap d = map_for(cost, {{4, 2}});
    FlowField flow;
    flow.compute(d);
    TileWorld tw;
    tw.resize(5, 3, 16, {100, 200});
    CHECK(flow.next_cell({4, 2}) == IVec2{4, 2});
    CHECK(flow.next_cell({0, 2}) == IVec2{1, 2});
    CHECK(flow.next_cell({1, 1}) == IVec2{1, 1}); // a wall: no flow
    const Vec2 at_start = tw.tile_center({0, 2});
    const Vec2 dir = flow.direction(at_start, tw);
    CHECK(dir.x == doctest::Approx(1.0f));
    CHECK(dir.y == doctest::Approx(0.0f).epsilon(1e-6));
    // Off-centre: still points at the next centre, unit length.
    const Vec2 off = flow.direction(at_start + Vec2{-5, 6}, tw);
    CHECK(std::sqrt(off.x * off.x + off.y * off.y) == doctest::Approx(1.0f));
    CHECK(off.y < 0.0f);
    CHECK(flow.direction(tw.tile_center({4, 2}), tw) == Vec2{});
    CHECK(flow.direction({0, 0}, tw) == Vec2{});  // outside the map
    CHECK(flow.next_cell({0, 0}) == IVec2{1, 0}); // around the wall's top
    // No allocation once sized: recompute the map and the field each "turn".
    DijkstraMap live;
    live.resize({5, 3});
    const IVec2 goals[] = {{0, 0}};
    live.compute(cost, goals, GridDirs::Eight);
    flow.compute(live);
    CHECK_NO_ALLOC({
        for (int i = 0; i < 20; ++i) {
            const IVec2 g[] = {{i % 5, (i / 5) % 3 == 1 ? 0 : (i / 5) % 3}};
            live.compute(cost, g, i % 2 == 0 ? GridDirs::Eight : GridDirs::Four);
            flow.compute(live);
            (void)flow.direction({120, 210}, tw);
        }
    });
}
