// Autotiling: edge masks, the 47-tile blob set, variants and decorations. autotile_cell must
// always agree with a full autotile().
#include <array>
#include <cstdint>
#include <set>
#include <vector>

#include <lam/math/rng.h>
#include <lam/test/support.h>
#include <lam/tilemap/autotile.h>

using namespace lam;

namespace {

// Terrain from rows: '.' = 1 (grass), '~' = 2 (water), 's' = 3 (soil), '=' = 4 (bridge), else 0.
Grid<std::uint8_t> terrain_from(std::initializer_list<const char *> rows) {
    const int w = static_cast<int>(std::string_view(*rows.begin()).size());
    Grid<std::uint8_t> g(w, static_cast<int>(rows.size()), 0);
    int y = 0;
    for (const char *row : rows) {
        for (int x = 0; x < w; ++x) {
            switch (row[x]) {
            case '.':
                g(x, y) = 1;
                break;
            case '~':
                g(x, y) = 2;
                break;
            case 's':
                g(x, y) = 3;
                break;
            case '=':
                g(x, y) = 4;
                break;
            default:
                break;
            }
        }
        ++y;
    }
    return g;
}

AutotileRule rule(std::uint8_t value, AutotileMode mode, TileId first, std::uint8_t variants = 1,
                  bool edge_outside = true) {
    AutotileRule r;
    r.terrain_value = value;
    r.mode = mode;
    r.first = first;
    r.variants = variants;
    r.edge_outside = edge_outside;
    return r;
}

} // namespace

// ---- basics -------------------------------------------------------------------------------------

TEST_CASE("edge masks mark the sides that differ") {
    // A plus of water: the centre has water on all four sides.
    const Grid<std::uint8_t> t = terrain_from({" ~ ", "~~~", " ~ "});
    CHECK(edge_mask4(t, {1, 1}, 2, false) == 0);
    CHECK(edge_mask4(t, {1, 0}, 2, false) == (kEdgeNorth | kEdgeEast | kEdgeWest));
    CHECK(edge_mask4(t, {2, 1}, 2, false) == (kEdgeNorth | kEdgeEast | kEdgeSouth));
    // Outside the grid may count as the same terrain (a lake running off the map edge).
    CHECK(edge_mask4(t, {1, 0}, 2, true) == (kEdgeEast | kEdgeWest));
}

TEST_CASE("a tileset parses and the ground layer picks edges and variants") {
    // Grass with a water pond and a tree on grass.
    const Grid<std::uint8_t> level = terrain_from({"..~~", "..~~", "...."});
    AutotileRule grass = rule(1, AutotileMode::Plain, 1, 3);
    grass.details = {12};
    grass.detail_percent = 100;
    const AutotileRule water = rule(2, AutotileMode::Edge4, 32);
    const std::vector<AutotileRule> rules = {grass, water};
    Grid<TileId> ground;
    autotile(level, rules, 42, ground);
    REQUIRE(ground.width() == 4);
    // Water at (2, 0): north is outside (counts as water), east water, south water, west grass.
    CHECK(ground(2, 0) == 32 + kEdgeWest);
    // Water at (3, 1): its south (3, 2) is grass.
    CHECK(ground(3, 1) == 32 + kEdgeSouth);
    const TileId g = ground(0, 0);
    CHECK((g >= 1 && g <= 3));
    // Same seed, same picture.
    Grid<TileId> again;
    autotile(level, rules, 42, again);
    CHECK(again == ground);
    Grid<std::uint8_t> blocked(4, 3, 0);
    blocked(1, 1) = 1; // the tree
    Grid<TileId> details;
    autotile_details(level, rules, 42, details, &blocked);
    CHECK(details(0, 0) == 12);
    CHECK(details(1, 1) == 0); // blocked by the tree
    CHECK(details(2, 0) == 0); // water has none
}

TEST_CASE("changing terrain updates the tile and its neighbours") {
    Grid<std::uint8_t> map(5, 5, 1);
    const std::vector<AutotileRule> rules = {rule(1, AutotileMode::Plain, 1),
                                             rule(3, AutotileMode::Edge4, 64, 1, false)};
    Grid<TileId> layer;
    autotile(map, rules, 1, layer);
    CHECK(layer(2, 2) == 1);
    map(2, 2) = 3;
    autotile_cell(map, rules, 1, {2, 2}, layer);
    CHECK(layer(2, 2) == 64 + 15); // alone: edges all round
    map(3, 2) = 3;
    autotile_cell(map, rules, 1, {3, 2}, layer);
    CHECK(layer(2, 2) == 64 + (15 & ~kEdgeEast));
    CHECK(layer(3, 2) == 64 + (15 & ~kEdgeWest));
}

// ---- blob set, variants, blends ----------------------------------------------------------------

TEST_CASE("autotile: the blob set has exactly 47 reduced masks, ranked in mask order") {
    std::set<int> seen;
    int previous = -1;
    for (int m = 0; m < 256; ++m) {
        const int i = blob47_index(m);
        if (i < 0) continue;
        CHECK(i == previous + 1);
        previous = i;
        seen.insert(i);
    }
    CHECK(seen.size() == 47);
    CHECK(blob47_index(0) == 0);
    CHECK(blob47_index(255) == 46);
    CHECK(blob47_index(kBlobNE) == -1); // a corner without its sides is never produced
    CHECK(blob47_index(-1) == -1);
    CHECK(blob47_index(256) == -1);
    // Every mask blob_mask47 produces is reduced.
    Rng rng(5);
    Grid<std::uint8_t> t(12, 12, 0);
    for (std::uint8_t &c : t.cells())
        c = static_cast<std::uint8_t>(rng.below(2));
    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x < 12; ++x) {
            CHECK(blob47_index(blob_mask47(t, {x, y}, 1, true)) >= 0);
            CHECK(blob47_index(blob_mask47(t, {x, y}, 1, false)) >= 0);
        }
    }
}

TEST_CASE("autotile: blob masks count a corner only when both of its sides match") {
    const Grid<std::uint8_t> t = terrain_from({"~~.", "~~~", ".~."});
    // (1, 1): N, E, S, W water; NW water (with N and W); NE grass; SE, SW grass.
    CHECK(blob_mask47(t, {1, 1}, 2, false) == (kBlobN | kBlobE | kBlobS | kBlobW | kBlobNW));
    // (0, 0): E and S water, SE water, the rest outside (not same).
    CHECK(blob_mask47(t, {0, 0}, 2, false) == (kBlobE | kBlobS | kBlobSE));
    // (0, 0) with outside counted as water: N, W, NW, NE, SW outside; E, S, SE water -> surrounded.
    CHECK(blob_mask47(t, {0, 0}, 2, true) == 255);
    const Grid<std::uint8_t> isolated = terrain_from({"...", ".~.", "..."});
    CHECK(blob_mask47(isolated, {1, 1}, 2, true) == 0);
    const std::vector<AutotileRule> rules = {rule(2, AutotileMode::Blob47, 100)};
    Grid<TileId> out;
    autotile(t, rules, 0, out);
    CHECK(out(1, 1) ==
          100u + static_cast<TileId>(blob47_index(kBlobN | kBlobE | kBlobS | kBlobW | kBlobNW)));
    CHECK(out(2, 0) == 0); // grass has no rule
    autotile(isolated, rules, 0, out);
    CHECK(out(1, 1) == 100); // isolated: rank 0
}

TEST_CASE("autotile: variants spread deterministically and depend on the seed") {
    const Grid<std::uint8_t> t(32, 32, 1);
    const std::vector<AutotileRule> rules = {rule(1, AutotileMode::Edge4, 10, 4)};
    Grid<TileId> a;
    autotile(t, rules, 7, a);
    std::array<int, 4> counts{};
    for (int y = 1; y < 31; ++y) {
        for (int x = 1; x < 31; ++x) {
            const TileId v = a(x, y);
            REQUIRE(v >= 10);
            REQUIRE(v < 14); // mask 0 (inner cells): first + 0 * 4 + variant
            ++counts[v - 10];
        }
    }
    for (const int c : counts)
        CHECK(c > 150); // about 225 each
    Grid<TileId> b;
    autotile(t, rules, 8, b);
    CHECK(b != a);
    CHECK(tile_hash(3, 4, 7) == tile_hash(3, 4, 7));
    CHECK(tile_hash(3, 4, 7) != tile_hash(4, 3, 7));
    CHECK(tile_hash(-3, 4, 7) != tile_hash(3, 4, 7));
    // Edge cells on an inside-out grid: edges multiply by the variant count.
    const std::vector<AutotileRule> fenced = {rule(1, AutotileMode::Edge4, 10, 4, false)};
    autotile(t, fenced, 7, a);
    const TileId corner = a(0, 0);
    CHECK(corner >= 10u + (kEdgeNorth | kEdgeWest) * 4u);
    CHECK(corner < 10u + (kEdgeNorth | kEdgeWest) * 4u + 4u);
}

TEST_CASE("autotile: blends count as the same terrain for edges") {
    const Grid<std::uint8_t> t = terrain_from({"~=~"});
    AutotileRule water = rule(2, AutotileMode::Edge4, 0, 1, false);
    water.first = 50;
    std::vector<AutotileRule> rules = {water};
    Grid<TileId> out;
    autotile(t, rules, 0, out);
    CHECK(out(0, 0) == 50u + (kEdgeNorth | kEdgeSouth | kEdgeWest | kEdgeEast));
    rules[0].blends = {4};
    autotile(t, rules, 0, out);
    CHECK(out(0, 0) == 50u + (kEdgeNorth | kEdgeSouth | kEdgeWest));
    CHECK(out(1, 0) == 0); // the bridge itself has no rule
}

TEST_CASE("autotile: autotile_cell after any edit equals a full autotile, without allocating") {
    Rng rng(1234);
    Grid<std::uint8_t> t(20, 14, 1);
    for (std::uint8_t &c : t.cells())
        c = static_cast<std::uint8_t>(1 + rng.below(3));
    AutotileRule water = rule(2, AutotileMode::Blob47, 200, 2, false);
    water.blends = {4};
    const std::vector<AutotileRule> rules = {rule(1, AutotileMode::Plain, 1, 3), water,
                                             rule(3, AutotileMode::Edge4, 100, 2, true)};
    Grid<TileId> layer;
    autotile(t, rules, 99, layer);
    Grid<TileId> full;
    for (int edit = 0; edit < 300; ++edit) {
        const IVec2 c{static_cast<int>(rng.below(20)), static_cast<int>(rng.below(14))};
        t(c.x, c.y) = static_cast<std::uint8_t>(rng.below(5)); // 0 (none) and 4 (bridge) included
        autotile_cell(t, rules, 99, c, layer);
        if (edit % 25 == 0) {
            autotile(t, rules, 99, full);
            REQUIRE(full == layer);
        }
    }
    autotile(t, rules, 99, full);
    CHECK(full == layer);
    CHECK_NO_ALLOC({
        for (int i = 0; i < 100; ++i)
            autotile_cell(t, rules, 99, {i % 20, i % 14}, layer);
        autotile(t, rules, 99, layer); // same size: no resize
    });
    // Edits on the border and outside the grid are safe.
    autotile_cell(t, rules, 99, {0, 0}, layer);
    autotile_cell(t, rules, 99, {19, 13}, layer);
    autotile_cell(t, rules, 99, {-5, 40}, layer);
    autotile(t, rules, 99, full);
    CHECK(full == layer);
}
