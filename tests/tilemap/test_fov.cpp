// Field of view: symmetric shadowcasting and FovMemory.
#include <cstdint>
#include <string>
#include <vector>

#include <lam/core/data.h>
#include <lam/core/hash.h>
#include <lam/math/rng.h>
#include <lam/physics/tile_world.h>
#include <lam/test/support.h>
#include <lam/tilemap/fov.h>

using namespace lam;

namespace {

// '#' = opaque wall, anything else open.
Grid<std::uint8_t> walls_from(std::initializer_list<const char *> rows) {
    const int w = static_cast<int>(std::string_view(*rows.begin()).size());
    Grid<std::uint8_t> g(w, static_cast<int>(rows.size()), 0);
    int y = 0;
    for (const char *row : rows) {
        for (int x = 0; x < w; ++x)
            g(x, y) = row[x] == '#' ? 1 : 0;
        ++y;
    }
    return g;
}

// Outside the grid is opaque.
bool grid_opaque(const void *ctx, IVec2 c) {
    return static_cast<const Grid<std::uint8_t> *>(ctx)->get_or(c, 1) != 0;
}

Grid<std::uint8_t> fov_of(const Grid<std::uint8_t> &walls, IVec2 origin, int radius) {
    Grid<std::uint8_t> vis(walls.width(), walls.height(), 0);
    compute_fov(origin, radius, &grid_opaque, &walls, vis);
    return vis;
}

TileWorld tile_world_from(const Grid<std::uint8_t> &walls) {
    TileWorld tw;
    tw.resize(walls.width(), walls.height(), 16, {0, 0});
    for (int y = 0; y < walls.height(); ++y) {
        for (int x = 0; x < walls.width(); ++x) {
            if (walls(x, y) != 0) tw.set({x, y}, TileShape::Solid, kTileOpaque);
        }
    }
    return tw;
}

int count(const Grid<std::uint8_t> &g) {
    int n = 0;
    for (const std::uint8_t c : g.cells())
        n += c != 0 ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("fov: an open field shows exactly the disc dx*dx + dy*dy <= r * (r + 1)") {
    const Grid<std::uint8_t> open(41, 41, 0);
    for (const int r : {0, 1, 2, 5, 8, 13}) {
        const Grid<std::uint8_t> vis = fov_of(open, {20, 20}, r);
        for (int y = 0; y < 41; ++y) {
            for (int x = 0; x < 41; ++x) {
                const int dx = x - 20;
                const int dy = y - 20;
                INFO("r " << r << " cell " << x << ", " << y);
                CHECK((vis(x, y) != 0) == (dx * dx + dy * dy <= r * (r + 1)));
            }
        }
    }
    Grid<std::uint8_t> vis(41, 41, 7);
    compute_fov({20, 20}, -1, &grid_opaque, &open, vis);
    CHECK(count(vis) == 0); // negative radius: nothing, and the grid is cleared
}

TEST_CASE("fov: walls are lit, and a pillar casts a shadow behind it") {
    const Grid<std::uint8_t> room = walls_from({
        "#########",
        "#.......#",
        "#.......#",
        "#...#...#",
        "#.......#",
        "#########",
    });
    const Grid<std::uint8_t> vis = fov_of(room, {1, 3}, 20);
    // Every wall cell bordering the room is lit except the ones the pillar hides.
    CHECK(vis(0, 3) == 1);
    CHECK(vis(0, 0) == 1);
    CHECK(vis(8, 1) == 1);
    CHECK(vis(4, 3) == 1); // the pillar itself
    CHECK(vis(5, 3) == 0); // straight behind it
    CHECK(vis(6, 3) == 0);
    CHECK(vis(8, 3) == 0); // the far wall in its shadow
    CHECK(vis(7, 1) == 1); // seen past its top
    CHECK(vis(1, 3) == 1); // the origin
    // Nothing outside the room is ever marked (the grid ends at the walls).
    const Grid<std::uint8_t> boxed = walls_from({"###", "#.#", "###"});
    CHECK(count(fov_of(boxed, {1, 1}, 5)) == 9);
}

TEST_CASE("fov: if A sees B then B sees A (symmetric shadowcasting)") {
    Rng rng(77);
    for (int map = 0; map < 6; ++map) {
        Grid<std::uint8_t> walls(24, 18, 0);
        for (std::uint8_t &c : walls.cells())
            c = rng.below(100) < 28 ? 1 : 0;
        std::vector<IVec2> floor;
        for (int y = 0; y < 18; ++y) {
            for (int x = 0; x < 24; ++x) {
                if (walls(x, y) == 0) floor.push_back({x, y});
            }
        }
        REQUIRE(floor.size() > 10);
        std::vector<Grid<std::uint8_t>> seen;
        seen.reserve(floor.size());
        for (const IVec2 a : floor)
            seen.push_back(fov_of(walls, a, 9));
        int pairs = 0;
        for (std::size_t i = 0; i < floor.size(); ++i) {
            for (std::size_t j = i + 1; j < floor.size(); ++j) {
                const IVec2 a = floor[i];
                const IVec2 b = floor[j];
                const bool ab = seen[i](b.x, b.y) != 0;
                const bool ba = seen[j](a.x, a.y) != 0;
                if (ab != ba) {
                    INFO("map " << map << ": " << a.x << "," << a.y << " vs " << b.x << "," << b.y);
                    CHECK(ab == ba);
                }
                pairs += ab ? 1 : 0;
            }
        }
        CHECK(pairs > 100); // make sure the property was exercised
    }
}

TEST_CASE("fov: an origin near or beyond the edge is safe and marks only inside cells") {
    const Grid<std::uint8_t> open(10, 6, 0);
    Grid<std::uint8_t> vis(10, 6, 0);
    const auto none = [](const void *, IVec2) { return false; };
    compute_fov({0, 0}, 4, none, nullptr, vis);
    CHECK(vis(0, 0) == 1);
    CHECK(vis(4, 0) == 1);
    CHECK(vis(5, 0) == 0);
    compute_fov({-3, 2}, 5, none, nullptr,
                vis); // outside: sees into the grid, marks no outside cell
    CHECK(vis(0, 2) == 1);
    CHECK(vis(9, 2) == 0);
    compute_fov({500, -500}, 1000000, none, nullptr, vis); // huge radius: bounded by the grid
    CHECK(count(vis) == 60);
    Grid<std::uint8_t> empty;
    compute_fov({0, 0}, 5, none, nullptr, empty); // a 0 x 0 grid
    CHECK(empty.size() == 0);
}

TEST_CASE("fov memory: explored accumulates, doors toggle sight, extra opacity blocks") {
    const Grid<std::uint8_t> walls = walls_from({
        "##########",
        "#....#...#",
        "#....D...#",
        "#....#...#",
        "##########",
    });
    TileWorld tw = tile_world_from(walls);
    tw.set({5, 2}, TileShape::Solid, kTileOpaque); // the closed door
    FovMemory fov;
    update_fov(fov, tw, {2, 2}, 12);
    CHECK(fov.visible.width() == 10);
    CHECK(fov.visible(4, 2) == 1);
    CHECK(fov.visible(5, 2) == 1); // the door itself is seen
    CHECK(fov.visible(7, 2) == 0); // the far room is not
    CHECK(fov.explored(7, 2) == 0);
    // Open the door: the far room becomes visible and explored.
    tw.set({5, 2}, TileShape::Empty, 0);
    update_fov(fov, tw, {2, 2}, 12);
    CHECK(fov.visible(7, 2) == 1);
    CHECK(fov.explored(7, 2) == 1);
    // Walk into the far room and look back with the door closed: the first room stays explored.
    tw.set({5, 2}, TileShape::Solid, kTileOpaque);
    update_fov(fov, tw, {7, 2}, 12);
    CHECK(fov.visible(2, 2) == 0);
    CHECK(fov.explored(2, 2) == 1);
    CHECK(fov.visible(8, 1) == 1);
    // Extra opacity (smoke): a cloud between the walker and the wall.
    Grid<std::uint8_t> smoke(10, 5, 0);
    smoke(7, 1) = 1;
    smoke(7, 3) = 1;
    smoke(8, 2) = 1;
    update_fov(fov, tw, {7, 2}, 12, &smoke);
    CHECK(fov.visible(8, 2) == 1); // the smoke cell itself is seen
    CHECK(fov.visible(9, 2) == 0); // the wall behind it is not
    // Steady state: no allocation once the grids have the world's size.
    CHECK_NO_ALLOC({
        for (int i = 0; i < 50; ++i)
            update_fov(fov, tw, {1 + i % 4, 1 + i % 3}, 8);
    });
    // A new world size resizes and forgets.
    TileWorld bigger;
    bigger.resize(12, 7, 16, {});
    update_fov(fov, bigger, {6, 3}, 2);
    CHECK(fov.explored.width() == 12);
    CHECK(fov.explored(1, 1) == 0);
}

TEST_CASE("fov memory: saves round-trip and the hash covers both grids") {
    FovMemory fov;
    const Grid<std::uint8_t> walls = walls_from({"#######", "#..#..#", "#.....#", "#######"});
    const TileWorld tw = tile_world_from(walls);
    update_fov(fov, tw, {1, 1}, 6);
    update_fov(fov, tw, {5, 2}, 1);
    DataWriter out;
    lam_write(fov, out, SerializeContext{});
    const Result<std::string> json = out.finish();
    REQUIRE(json.ok());
    Result<DataDocument> doc = DataDocument::parse(*json, "save.json");
    REQUIRE(doc.ok());
    FovMemory back;
    DataReader root = doc->root();
    REQUIRE(lam_read(back, root, SerializeContext{}).ok());
    REQUIRE(doc->finish().ok());
    CHECK(back.visible == fov.visible);
    CHECK(back.explored == fov.explored);
    const auto hash = [](const FovMemory &m) {
        Hasher h;
        lam_hash(m, h);
        return h.value();
    };
    CHECK(hash(back) == hash(fov));
    back.explored(3, 2) ^= 1;
    CHECK(hash(back) != hash(fov));
    // Mismatched grids are refused.
    Result<DataDocument> bad = DataDocument::parse(
        R"({"visible": {"w": 1, "h": 1, "runs": [0, 1]}, "explored": {"w": 2, "h": 1, "runs": [0, 2]}})",
        "bad.json");
    REQUIRE(bad.ok());
    FovMemory m;
    DataReader r = bad->root();
    CHECK_FALSE(lam_read(m, r, SerializeContext{}).ok());
}
