// Tilemap data: native and Tiled maps, tilesets, GIDs, TileWorld filling and TilemapInstance.
#include <clocale>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <lam/core/data.h>
#include <lam/core/hash.h>
#include <lam/physics/tile_world.h>
#include <lam/test/support.h>
#include <lam/tilemap/grid_path.h>
#include <lam/tilemap/tilemap.h>
#include <lam/tilemap/tileset.h>

using namespace lam;
using namespace lam::literals;

namespace {

// The embedded tilesets of a parsed map, in TilesetRef order (external ones null).
std::vector<const Tileset *> embedded_sets(const TilemapAsset &m) {
    std::vector<const Tileset *> out;
    for (const TilesetRef &ref : m.tilesets) {
        out.push_back(ref.embedded >= 0
                          ? &m.embedded_tilesets[static_cast<std::size_t>(ref.embedded)]
                          : nullptr);
    }
    return out;
}

TileWorld world_of(const TilemapAsset &m, Vec2 origin = {}) {
    TileWorld tw;
    const std::vector<const Tileset *> sets = embedded_sets(m);
    fill_tile_world(m, m.layers, sets, origin, tw);
    return tw;
}

std::string error_of(std::string_view text, std::string_view file = "bad.json") {
    Result<TilemapAsset> r = parse_native_map(text, file);
    return r.ok() ? std::string() : r.error().message;
}

const char *kLevel = R"({
  "legend": { ".": "grass", "~": "water", "T": { "terrain": "grass", "object": "tree" } },
  "rows": [ "..~~", ".T~~", "...." ],
  "objects": [ { "type": "house", "x": 0, "y": 2, "door": "south", "size": 3 } ],
  "properties": { "music": "day" }
})";

} // namespace

// ---- ASCII maps ---------------------------------------------------------------------------------

TEST_CASE("tile map: parsing, lookup, find and collision") {
    const Result<TilemapAsset> r = parse_native_map("\n#####\n#P.A#\n#..\n#####\n", "level.txt");
    REQUIRE(r.ok());
    const TilemapAsset &m = *r;
    CHECK(m.width == 5);
    CHECK(m.height == 4);
    CHECK(m.terrain(1, 1) == terrain_value(m, "P"_id));
    CHECK(m.terrain(3, 2) == 0); // padded short row
    CHECK(m.terrain.get_or(-1, 0, 0) == 0);
    std::vector<IVec2> apples;
    find_cells(m.terrain, terrain_value(m, "A"_id), apples);
    REQUIRE(apples.size() == 1);
    CHECK(apples[0] == IVec2{3, 1});
    const TileWorld grid = world_of(m);
    CHECK(grid.at({0, 0}) == TileShape::Solid);
    CHECK(grid.at({1, 1}) == TileShape::Empty);
    CHECK(grid.at({5, 1}) == TileShape::Solid); // outside is solid
    CHECK(grid.cost(0, 0) == 0);
    CHECK(grid.cost(1, 1) == 10);
    CHECK(grid.opaque({0, 0}));
    CHECK_FALSE(grid.opaque({2, 1}));
    CHECK_FALSE(parse_native_map("\n  \n", "empty.txt").ok());
}

TEST_CASE("a level parses terrain, objects and properties") {
    const Result<TilemapAsset> parsed = parse_native_map(kLevel, "level.json");
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    const TilemapAsset &map = *parsed;
    CHECK(map.width == 4);
    CHECK(map.height == 3);
    const std::uint8_t grass = terrain_value(map, "grass"_id);
    const std::uint8_t water = terrain_value(map, "water"_id);
    REQUIRE(grass != 0);
    REQUIRE(water != 0);
    CHECK(grass != water);
    CHECK(terrain_value(map, "lava"_id) == 0);
    CHECK(map.terrain(2, 0) == water);
    CHECK(map.terrain(1, 1) == grass); // the tree stands on grass
    CHECK(map.terrain.get_or(9, 9, 0) == 0);
    std::vector<IVec2> cells;
    find_cells(map.terrain, water, cells);
    CHECK(cells.size() == 4);
    const MapObject *tree = nullptr;
    const MapObject *house = nullptr;
    for (const MapObject &o : map.objects) {
        if (o.type == "tree"_id) tree = &o;
        if (o.type == "house"_id) house = &o;
    }
    REQUIRE(tree != nullptr);
    CHECK(tree->rect.x == 16); // cell (1, 1) of 16-pixel tiles
    CHECK(tree->rect.y == 16);
    REQUIRE(house != nullptr);
    CHECK(house->rect == Rect{0, 32, 16, 16});
    CHECK(prop_or(house->props, "door"_id) == "south");
    CHECK(prop_int(house->props, "size"_id) == 3);
    CHECK(prop_int(house->props, "missing"_id, 7) == 7);
    CHECK(prop_or(map.properties, "music"_id) == "day");
    // Collision comes from terrain properties: water declared solid.
    const std::string with_water =
        std::string(R"({"terrains": {"water": {"shape": "solid", "cost": 0}},)") +
        std::string(kLevel).substr(std::string(kLevel).find('{') + 1);
    const Result<TilemapAsset> solid = parse_native_map(with_water, "level.json");
    REQUIRE(solid.ok());
    const TileWorld grid = world_of(*solid);
    CHECK(grid.at({2, 0}) == TileShape::Solid);
    CHECK(grid.at({0, 0}) == TileShape::Empty);
    CHECK(grid.cost(2, 0) == 0);
}

TEST_CASE("level errors name the place") {
    CHECK(error_of(R"({"legend": {".": "g"}, "rows": [".x"]})").find("not in the legend") !=
          std::string::npos);
    CHECK(error_of(R"({"legend": {".": "g"}, "rows": ["..", "."]})").find("/rows/1") !=
          std::string::npos);
    CHECK(
        error_of(
            R"({"legend": {".": "g"}, "rows": [".."], "objects": [{"type": "a", "x": 5, "y": 0}]})")
            .find("outside") != std::string::npos);
    CHECK(error_of(R"({"legend": {"..": "g"}, "rows": [".."]})").find("one character") !=
          std::string::npos);
    CHECK(error_of("[]").find("bad.json") == 0);
    CHECK(error_of(R"({"legend": {".": "g"}, "rows": ["."], "colour": 3})").find("colour") !=
          std::string::npos);
    CHECK(
        error_of(R"({"legend": {".": "g"}, "rows": ["."], "terrains": {"g": {"shape": "round"}}})")
            .find("round") != std::string::npos);
    CHECK(error_of(
              R"({"legend": {".": "g"}, "rows": ["."], "terrains": {"g": {"flags": ["sticky"]}}})")
              .find("sticky") != std::string::npos);
    CHECK(error_of(R"({"width": 2, "height": 1, "layers": [{"name": "a", "data": [1]}]})")
              .find("holds 1") != std::string::npos);
    CHECK_FALSE(
        error_of(R"({"legend": {".": "g"}, "rows": [".."], "width": 3, "height": 1})").empty());
    CHECK_FALSE(error_of(R"({"width": 2, "height": 2, "autotile": {"rules": []}})").empty());
}

// ---- native maps --------------------------------------------------------------------------------

TEST_CASE("native map: ASCII rows make a hidden collision layer over an embedded terrain set") {
    const Result<TilemapAsset> r = parse_native_map("  ##\r\n#. \r\n", "room.txt");
    REQUIRE(r.ok());
    const TilemapAsset &m = *r;
    CHECK(m.width == 4);
    CHECK(m.height == 2);
    REQUIRE(m.tilesets.size() == 1);
    CHECK(m.tilesets[0].first_gid == 1);
    CHECK(m.tilesets[0].embedded == 0);
    REQUIRE(m.embedded_tilesets.size() == 1);
    const Tileset &set = m.embedded_tilesets[0];
    CHECK(set.count == 2);
    CHECK(set.regions.empty()); // props only: nothing to draw
    const TileLayer *terrain = find_layer(m.layers, "terrain"_id);
    REQUIRE(terrain != nullptr);
    CHECK(terrain->collision);
    CHECK_FALSE(terrain->visible);
    CHECK(terrain->tiles(0, 0) == 0);
    CHECK(terrain->tiles(2, 0) == 1); // '#' first used -> value 1 -> GID 1
    CHECK(terrain->tiles(1, 1) == 2);
    CHECK(set.prop(0).shape == TileShape::Solid);
    CHECK((set.prop(0).flags & kTileOpaque) != 0);
    CHECK(set.prop(1).shape == TileShape::Empty);
    CHECK(set.prop(5).cost == kDefaultTileCost); // out of range: defaults
    CHECK_FALSE(parse_native_map("ab\x01", "bad.txt").ok());
    CHECK(
        parse_native_map("\xEF\xBB\xBF{\"rows\": [\".\"], \"legend\": {\".\": \"g\"}}", "bom.json")
            .ok());
}

TEST_CASE("native map: legend tiles, terrain props, plain layers and tilesets") {
    const char *text = R"({
      "tile_size": 8,
      "tilesets": ["tiles.tileset.json", {"source": "deco.tsj", "first_gid": 65}],
      "terrains": {"wall": {"shape": "solid", "flags": ["opaque", "hazard"], "cost": 0, "tile": 3, "kind": "stone"},
                   "mud": {"cost": 40, "tile": 5}},
      "legend": {"#": "wall", ".": "grass", "~": "mud", "D": {"terrain": "grass", "object": "door", "tile": 9}},
      "rows": ["####", "#.D#", "#~.#", "####"],
      "layers": [{"name": "deco", "data": [0,0,0,0, 0,65,0,0, 0,0,66,0, 0,0,0,0], "collision": false,
                  "render_layer": 20, "opacity": 0.5, "parallax": [0.5, 1]}],
      "objects": [{"type": "chest", "x": 1, "y": 2, "w": 2, "h": 1, "name": "gold", "locked": true, "value": 2.5}],
      "properties": {"music": "cave", "dark": true, "level": 3}
    })";
    const Result<TilemapAsset> r = parse_native_map(text, "cave.map.json");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    const TilemapAsset &m = *r;
    CHECK(m.tile_size == 8);
    REQUIRE(m.tilesets.size() == 3);
    CHECK(m.tilesets[0].source == "tiles.tileset.json");
    CHECK(m.tilesets[0].first_gid == 1);
    CHECK(m.tilesets[1].source == "deco.tsj");
    CHECK(m.tilesets[1].first_gid == 65);
    CHECK(m.tilesets[2].first_gid == (1u << 24)); // the terrain set sits above every real tileset
    CHECK(m.tilesets[2].embedded == 0);
    // Visible "ground" from the legend's and terrains' tiles.
    const TileLayer *ground = find_layer(m.layers, "ground"_id);
    REQUIRE(ground != nullptr);
    CHECK(ground->tiles(0, 0) == 3);
    CHECK(ground->tiles(1, 2) == 5);
    CHECK(ground->tiles(2, 1) == 9); // the legend's own tile beats the terrain's
    CHECK(ground->tiles(1, 1) == 0); // grass has no tile
    const TileLayer *deco = find_layer(m.layers, "deco"_id);
    REQUIRE(deco != nullptr);
    CHECK_FALSE(deco->collision);
    CHECK(deco->render_layer == 20);
    CHECK(deco->opacity == doctest::Approx(0.5f));
    CHECK(deco->parallax == Vec2{0.5f, 1});
    CHECK(deco->tiles(1, 1) == 65);
    // Objects: the legend's door and the listed chest, in tiles of 8 pixels.
    REQUIRE(m.objects.size() == 2);
    CHECK(m.objects[0].type == "door"_id);
    CHECK(m.objects[0].rect == Rect{16, 8, 8, 8});
    CHECK(m.objects[1].type == "chest"_id);
    CHECK(m.objects[1].name == "gold"_id);
    CHECK(m.objects[1].rect == Rect{8, 16, 16, 8});
    CHECK(prop_bool(m.objects[1].props, "locked"_id));
    CHECK(prop_float(m.objects[1].props, "value"_id) == doctest::Approx(2.5f));
    CHECK(prop_or(m.properties, "music"_id) == "cave");
    CHECK(prop_bool(m.properties, "dark"_id));
    CHECK(prop_int(m.properties, "level"_id) == 3);
    // Terrain props reach the TileWorld through the embedded set (external sets not loaded here).
    std::vector<const Tileset *> sets = embedded_sets(m);
    TileWorld tw;
    fill_tile_world(m, m.layers, sets, {100, 50}, tw);
    CHECK(tw.tile_size == 8);
    CHECK(tw.origin == Vec2{100, 50});
    CHECK(tw.at({0, 0}) == TileShape::Solid);
    CHECK(tw.flags_at({0, 0}) == (kTileOpaque | kTileHazard));
    CHECK(tw.cost(1, 2) == 40);
    CHECK(tw.cost(1, 1) == 10);
    CHECK(m.embedded_tilesets[0].prop(terrain_value(m, "wall"_id) - 1u).kind == "stone"_id);
    // terrain_first_gid must stay above the real tilesets.
    CHECK_FALSE(
        error_of(
            R"({"tilesets": ["a.tsj"], "terrain_first_gid": 1, "legend": {".": "g"}, "rows": ["."]})")
            .empty());
    CHECK_FALSE(error_of(R"({"tilesets": ["a.tsj", "b.tsj"], "width": 1, "height": 1})").empty());
}

TEST_CASE("native map: autotile rules build the ground and details layers") {
    const char *text = R"({
      "legend": {".": "grass", "~": "water", "=": "bridge"},
      "rows": ["....", ".~~.", ".~=.", "...."],
      "autotile": {"layer": "ground", "seed": 7, "details_layer": "deco", "rules": [
        {"terrain": "grass", "mode": "plain", "first": 1, "variants": 3, "details": [40, 41], "detail_percent": 100},
        {"terrain": "water", "mode": "edge4", "first": 33, "blends": ["bridge"], "edge_outside": false}
      ]}
    })";
    const Result<TilemapAsset> r = parse_native_map(text, "lake.map.json");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    const TilemapAsset &m = *r;
    REQUIRE(m.autotile.size() == 2);
    CHECK(m.autotile_layer == "ground"_id);
    CHECK(m.autotile_seed == 7);
    const TileLayer *ground = find_layer(m.layers, "ground"_id);
    REQUIRE(ground != nullptr);
    // Water at (1, 1): grass north and west; east water, south water.
    CHECK(ground->tiles(1, 1) == 33u + (kEdgeNorth | kEdgeWest));
    // Water at (1, 2): the bridge east blends; grass west and south.
    CHECK(ground->tiles(1, 2) == 33u + (kEdgeWest | kEdgeSouth));
    for (const IVec2 c : {IVec2{0, 0}, IVec2{3, 3}, IVec2{0, 2}}) {
        const TileId t = ground->tiles(c.x, c.y);
        CHECK(t >= 1);
        CHECK(t <= 3);
    }
    CHECK(ground->tiles(2, 2) == 0); // no rule for the bridge
    const TileLayer *deco = find_layer(m.layers, "deco"_id);
    REQUIRE(deco != nullptr);
    CHECK((deco->tiles(0, 0) == 40 || deco->tiles(0, 0) == 41));
    CHECK(deco->tiles(1, 1) == 0); // water has no details
    CHECK_FALSE(
        error_of(
            R"({"legend": {".": "g"}, "rows": ["."], "autotile": {"rules": [{"terrain": "x", "first": 1}]}})")
            .empty());
    CHECK_FALSE(
        error_of(
            R"({"legend": {".": "g"}, "rows": ["."], "autotile": {"rules": [{"terrain": "g", "first": 1, "mode": "hex"}]}})")
            .empty());
}

// ---- Tiled import -------------------------------------------------------------------------------

namespace {

const char *kTiledMap = R"({
  "type": "map", "version": "1.10", "tiledversion": "1.10.2", "orientation": "orthogonal",
  "renderorder": "right-down", "infinite": false, "width": 4, "height": 3, "tilewidth": 16, "tileheight": 16,
  "nextlayerid": 9, "nextobjectid": 4, "compressionlevel": -1, "editorsettings": {"export": {"target": "."}},
  "properties": [{"name": "music", "type": "string", "value": "forest"},
                 {"name": "gravity", "type": "float", "value": 9.5},
                 {"name": "night", "type": "bool", "value": true},
                 {"name": "spawn", "type": "class", "propertytype": "P", "value": {}}],
  "tilesets": [
    {"firstgid": 1, "name": "ground", "tilewidth": 16, "tileheight": 16, "tilecount": 8, "columns": 4,
     "image": "tiles.png", "imagewidth": 64, "imageheight": 32, "margin": 0, "spacing": 0,
     "tiles": [{"id": 1, "type": "wall", "properties": [{"name": "shape", "type": "string", "value": "solid"},
                                                        {"name": "opaque", "type": "bool", "value": true},
                                                        {"name": "cost", "type": "int", "value": 0}]},
               {"id": 2, "properties": [{"name": "shape", "type": "string", "value": "slope_up_right"}]},
               {"id": 4, "animation": [{"tileid": 4, "duration": 100}, {"tileid": 5, "duration": 100},
                                       {"tileid": 6, "duration": 100}]}]},
    {"firstgid": 9, "source": "deco.tsj"}
  ],
  "layers": [
    {"id": 1, "name": "ground", "type": "tilelayer", "width": 4, "height": 3, "x": 0, "y": 0,
     "opacity": 1, "visible": true, "encoding": "csv", "data": "1,1,1,1,\n1,2,3,1,\n1,1,1,2147483651"},
    {"id": 2, "name": "deco", "type": "tilelayer", "width": 4, "height": 3, "opacity": 1, "visible": true,
     "properties": [{"name": "collision", "type": "bool", "value": false},
                    {"name": "render_layer", "type": "int", "value": 70}],
     "data": [0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 10, 0]},
    {"id": 3, "name": "folder", "type": "group", "opacity": 0.5, "visible": false, "layers": [
      {"id": 4, "name": "inner", "type": "tilelayer", "width": 4, "height": 3, "opacity": 0.5, "visible": true,
       "parallaxx": 0.5, "encoding": "base64", "data": "AgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}]},
    {"id": 5, "name": "things", "type": "objectgroup", "draworder": "topdown", "objects": [
      {"id": 1, "name": "start", "type": "spawn", "x": 8, "y": 24, "width": 16, "height": 8, "rotation": 0,
       "visible": true, "properties": [{"name": "facing", "type": "string", "value": "left"}]},
      {"id": 2, "name": "", "class": "coin", "gid": 5, "x": 32, "y": 48, "width": 16, "height": 16,
       "rotation": 0, "visible": true}]},
    {"id": 6, "name": "sky", "type": "imagelayer", "image": "sky.png"}
  ]
})";

std::string tiled_with(std::string_view from, std::string_view to) {
    std::string s = kTiledMap;
    const std::size_t at = s.find(from);
    REQUIRE(at != std::string::npos);
    s.replace(at, from.size(), to);
    return s;
}

ErrorCode code_of(const Result<TilemapAsset> &r) {
    return r.ok() ? ErrorCode::Unknown : r.error().code;
}

} // namespace

TEST_CASE("tiled: a map imports layers, tilesets, objects and properties") {
    const Result<TilemapAsset> r = parse_tiled_json(kTiledMap, "forest.tmj");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    const TilemapAsset &m = *r;
    CHECK(m.width == 4);
    CHECK(m.height == 3);
    CHECK(m.tile_size == 16);
    REQUIRE(m.tilesets.size() == 2);
    CHECK(m.tilesets[0].embedded == 0);
    CHECK(m.tilesets[1].source == "deco.tsj");
    CHECK(m.tilesets[1].first_gid == 9);
    REQUIRE(m.embedded_tilesets.size() == 1);
    const Tileset &ground_set = m.embedded_tilesets[0];
    CHECK(ground_set.name == "ground"_id);
    CHECK(ground_set.columns == 4);
    CHECK(ground_set.count == 8);
    CHECK(ground_set.image_path == "tiles.png");
    CHECK(ground_set.image_size == IVec2{64, 32});
    CHECK(ground_set.prop(1).shape == TileShape::Solid);
    CHECK(ground_set.prop(1).cost == 0);
    CHECK(ground_set.prop(1).kind == "wall"_id);
    CHECK((ground_set.prop(1).flags & kTileOpaque) != 0);
    CHECK(ground_set.prop(2).shape == TileShape::SlopeUpRight);
    CHECK(ground_set.prop(4).anim_first == 4);
    CHECK(ground_set.prop(4).anim_count == 3);
    CHECK(ground_set.prop(4).anim_ms == 100);
    // Tile layers keep their order; the group is flattened with its visibility and opacity.
    REQUIRE(m.layers.size() == 3);
    CHECK(m.layers[0].name == "ground"_id);
    CHECK(m.layers[0].collision);
    CHECK(m.layers[0].render_layer == kLayerTiles);
    CHECK(m.layers[0].tiles(1, 1) == 2);
    CHECK(m.layers[0].tiles(3, 2) == (kTileFlipH | 3u)); // flip bits are kept
    CHECK(m.layers[1].name == "deco"_id);
    CHECK_FALSE(m.layers[1].collision);
    CHECK(m.layers[1].render_layer == 70);
    CHECK(m.layers[1].tiles(1, 0) == 9);
    CHECK(m.layers[2].name == "inner"_id);
    CHECK_FALSE(m.layers[2].visible);
    CHECK(m.layers[2].opacity == doctest::Approx(0.25f));
    CHECK(m.layers[2].parallax == Vec2{0.5f, 1});
    CHECK(m.layers[2].tiles(0, 0) == 2); // uncompressed base64, little endian
    CHECK(m.layers[2].tiles(1, 0) == 0);
    // Objects: rects in pixels; the tile object moves from its bottom-left anchor.
    REQUIRE(m.objects.size() == 2);
    CHECK(m.objects[0].type == "spawn"_id);
    CHECK(m.objects[0].name == "start"_id);
    CHECK(m.objects[0].rect == Rect{8, 24, 16, 8});
    CHECK(prop_or(m.objects[0].props, "facing"_id) == "left");
    CHECK(m.objects[1].type == "coin"_id);
    CHECK_FALSE(m.objects[1].name);
    CHECK(m.objects[1].rect == Rect{32, 32, 16, 16});
    // Map properties as text; class-typed values are skipped.
    CHECK(prop_or(m.properties, "music"_id) == "forest");
    CHECK(prop_float(m.properties, "gravity"_id) == doctest::Approx(9.5f));
    CHECK(prop_bool(m.properties, "night"_id));
    CHECK(prop_or(m.properties, "spawn"_id, "none") == "none");
    // Collision: wall tile solid, a horizontally flipped slope mirrors.
    std::vector<const Tileset *> sets = embedded_sets(m);
    TileWorld tw;
    fill_tile_world(m, m.layers, sets, {}, tw);
    CHECK(tw.at({1, 1}) == TileShape::Solid);
    CHECK(tw.cost(1, 1) == 0);
    CHECK(tw.opaque({1, 1}));
    CHECK(tw.at({1, 0}) == TileShape::Empty);
    CHECK(tw.cost(1, 0) == kDefaultTileCost);
    CHECK(tw.at({2, 1}) == TileShape::SlopeUpRight);
    CHECK(tw.at({3, 2}) == TileShape::SlopeUpLeft);
    CHECK(tw.at({0, 0}) == TileShape::Solid); // a hidden layer still collides (the wall at 0, 0)
}

TEST_CASE("tiled: unsupported features are Unsupported, malformed data is Parse") {
    CHECK(code_of(parse_tiled_json(tiled_with(R"("encoding": "base64")",
                                              R"("encoding": "base64", "compression": "zlib")"),
                                   "z.tmj")) == ErrorCode::Unsupported);
    CHECK(code_of(parse_tiled_json(
              tiled_with(R"("orientation": "orthogonal")", R"("orientation": "isometric")"),
              "iso.tmj")) == ErrorCode::Unsupported);
    CHECK(code_of(parse_tiled_json(tiled_with(R"("infinite": false)", R"("infinite": true)"),
                                   "inf.tmj")) == ErrorCode::Unsupported);
    CHECK(code_of(parse_tiled_json(tiled_with(R"("height": 3, "tilewidth": 16, "tileheight": 16)",
                                              R"("height": 3, "tilewidth": 16, "tileheight": 8)"),
                                   "rect.tmj")) == ErrorCode::Unsupported);
    CHECK(code_of(parse_tiled_json(tiled_with(R"("source": "deco.tsj")", R"("source": "deco.tsx")"),
                                   "x.tmj")) == ErrorCode::Unsupported);
    CHECK(code_of(parse_tiled_json(
              tiled_with(R"({"tileid": 6, "duration": 100})", R"({"tileid": 7, "duration": 100})"),
              "anim.tmj")) == ErrorCode::Unsupported);
    const Result<TilemapAsset> short_csv =
        parse_tiled_json(tiled_with("1,1,1,2147483651", "1,1,1"), "csv.tmj");
    CHECK(code_of(short_csv) == ErrorCode::Parse);
    CHECK(short_csv.error().message.find("csv.tmj") != std::string::npos);
    CHECK(code_of(parse_tiled_json(tiled_with("1,1,1,2147483651", "1,1,1,x"), "csv.tmj")) ==
          ErrorCode::Parse);
    CHECK(code_of(parse_tiled_json(tiled_with(R"("data": [0, 9, 0, 0)", R"("data": [0, 9, 0)"),
                                   "arr.tmj")) == ErrorCode::Parse);
    CHECK(code_of(parse_tiled_json(tiled_with(R"("firstgid": 9)", R"("firstgid": 1)"),
                                   "gid.tmj")) == ErrorCode::Parse);
    CHECK(code_of(parse_tiled_json("{\"width\": 2}", "w.tmj")) == ErrorCode::Parse);
    CHECK(code_of(parse_tiled_json("not json", "nj.tmj")) == ErrorCode::Parse);
}

TEST_CASE("tiled: a .tsj tileset reads geometry, tile properties and the atlas group") {
    const char *tsj =
        R"({"type": "tileset", "name": "dungeon", "tilewidth": 16, "tileheight": 16, "margin": 1,
      "spacing": 2, "image": "dungeon.png", "imagewidth": 56, "imageheight": 38, "tiledversion": "1.10.2",
      "properties": [{"name": "atlas", "type": "string", "value": "world"}],
      "tiles": [{"id": 3, "class": "door", "properties": [{"name": "opaque", "type": "bool", "value": true},
                                                         {"name": "water", "type": "bool", "value": false},
                                                         {"name": "loot", "type": "int", "value": 5}]}]})";
    const Result<Tileset> r = parse_tiled_tileset(tsj, "dungeon.tsj");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    const Tileset &t = *r;
    CHECK(t.tile_size == 16);
    CHECK(t.margin == 1);
    CHECK(t.spacing == 2);
    CHECK(t.columns == 3); // (56 - 2 + 2) / 18
    CHECK(t.count == 6);   // 3 columns x 2 rows
    CHECK(t.props.size() == 6);
    CHECK(t.atlas_group == "world"_id);
    CHECK(t.use_atlas);
    CHECK(t.prop(3).kind == "door"_id);
    CHECK(t.prop(3).flags == kTileOpaque);
    CHECK(t.prop(0).cost == kDefaultTileCost);
    const auto bad = [](std::string_view text) {
        const Result<Tileset> b = parse_tiled_tileset(text, "bad.tsj");
        return b.ok() ? ErrorCode::Unknown : b.error().code;
    };
    CHECK(bad(R"({"tilewidth": 16, "tileheight": 8})") == ErrorCode::Unsupported);
    CHECK(bad(R"({"tilewidth": 16, "tileheight": 16, "tiles": [{"id": 0, "image": "a.png"}]})") ==
          ErrorCode::Unsupported);
    CHECK(bad(R"({"tilewidth": 16, "tileheight": 16, "tilecount": 2, "tiles": [{"id": 5}]})") ==
          ErrorCode::Parse);
    CHECK(
        bad(R"({"tilewidth": 16, "tileheight": 16, "tiles": [{"id": 0, "properties": [{"name": "shape", "value": "blob"}]}]})") ==
        ErrorCode::Parse);
    CHECK(
        bad(R"({"tilewidth": 16, "tileheight": 16, "tiles": [{"id": 0, "properties": [{"name": "cost", "value": -3}]}]})") ==
        ErrorCode::Parse);
    const Result<Tileset> none = parse_tiled_tileset(
        R"({"tilewidth": 8, "tileheight": 8, "properties": [{"name": "atlas", "value": "none"}]})",
        "n.tsj");
    REQUIRE(none.ok());
    CHECK_FALSE(none->use_atlas);
}

TEST_CASE("native tileset: strict keys, flags, animation and props-only sets") {
    const char *text =
        R"({"image": "tiles.png", "tile_size": 8, "columns": 8, "count": 64, "atlas": "none",
      "tiles": {"3": {"shape": "one_way", "flags": ["ladder", "water"], "cost": 25, "kind": "rope",
                      "anim": {"count": 4, "ms": 120}}}})";
    const Result<Tileset> r = parse_native_tileset(text, "tiles.tileset.json");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    CHECK(r->tile_size == 8);
    CHECK(r->count == 64);
    CHECK_FALSE(r->use_atlas);
    const TileProps &p = r->prop(3);
    CHECK(p.shape == TileShape::OneWay);
    CHECK(p.flags == (kTileLadder | kTileWater));
    CHECK(p.cost == 25);
    CHECK(p.kind == "rope"_id);
    CHECK(p.anim_first == 3);
    CHECK(p.anim_count == 4);
    CHECK(p.anim_ms == 120);
    CHECK(r->region(3).size == Vec2{}); // not finalized: no regions
    CHECK(parse_native_tileset(R"({"count": 4})", "props.tileset.json").ok());
    CHECK_FALSE(parse_native_tileset(R"({"tile_size": 8})", "x.tileset.json")
                    .ok()); // neither image nor count
    CHECK_FALSE(
        parse_native_tileset(R"({"count": 4, "colour": 1})", "x.tileset.json").ok()); // strict
    CHECK_FALSE(parse_native_tileset(R"({"count": 4, "tiles": {"9": {}}})", "x.tileset.json").ok());
    CHECK_FALSE(parse_native_tileset(R"({"count": 4, "tiles": {"x": {}}})", "x.tileset.json").ok());
}

// ---- GIDs, TileWorld, costs ---------------------------------------------------------------------

TEST_CASE("resolve_gid finds the tileset with the largest first_gid not above the id") {
    TilemapAsset m;
    m.tilesets.push_back(TilesetRef{{}, 5, {}, -1});
    m.tilesets.push_back(TilesetRef{{}, 20, {}, -1});
    Tileset a;
    Tileset b;
    const Tileset *sets[] = {&a, &b};
    CHECK(resolve_gid(m, sets, 0).set == nullptr);
    CHECK(resolve_gid(m, sets, kTileFlipH).set == nullptr); // flips of nothing are nothing
    CHECK(resolve_gid(m, sets, 3).set == nullptr);          // below the first tileset
    TileRef r = resolve_gid(m, sets, 5);
    CHECK(r.set == &a);
    CHECK(r.local == 0);
    r = resolve_gid(m, sets, 19 | kTileFlipV | kTileFlipD);
    CHECK(r.set == &a);
    CHECK(r.local == 14);
    CHECK(r.flip == (kTileFlipV | kTileFlipD));
    r = resolve_gid(m, sets, 20);
    CHECK(r.set == &b);
    CHECK(r.local == 0);
    const Tileset *only_first[] = {&a};
    CHECK(resolve_gid(m, only_first, 25).set == nullptr); // a missing (not Ready) tileset
    CHECK(resolve_gid(m, only_first, 25).local == 5);
}

TEST_CASE("fill_tile_world: last shape wins, flags add up, costs take the worst") {
    TilemapAsset m;
    m.width = 3;
    m.height = 2;
    m.tile_size = 10;
    m.tilesets.push_back(TilesetRef{{}, 1, {}, -1});
    Tileset set;
    set.props.resize(6);
    set.props[0].shape = TileShape::Solid; // GID 1
    set.props[1].flags = kTileHazard;      // GID 2
    set.props[1].cost = 30;
    set.props[2].shape = TileShape::HalfSlopeUpRightLow; // GID 3
    set.props[2].flags = kTileWater;
    set.props[3].cost = 0;  // GID 4: impassable floor
    set.props[4].cost = 15; // GID 5
    const Tileset *sets[] = {&set};
    TileLayer base;
    base.collision = true;
    base.tiles.resize(3, 2, 0);
    base.tiles(0, 0) = 1;
    base.tiles(1, 0) = 2;
    base.tiles(2, 0) = 4;
    base.tiles(0, 1) = 5;
    TileLayer top;
    top.collision = true;
    top.tiles.resize(3, 2, 0);
    top.tiles(0, 0) = 3 | kTileFlipH; // a slope over the solid: mirrored, it wins
    top.tiles(1, 0) = 5;
    top.tiles(1, 1) = 2;
    TileLayer art; // not a collision layer
    art.tiles.resize(3, 2, 1);
    base.name = intern("base");
    top.name = intern("top");
    art.name = intern("art");
    m.layers = {base, top, art};
    TileWorld tw;
    fill_tile_world(m, m.layers, sets, {5, 7}, tw);
    CHECK(tw.version == 1); // one change: resize + fill
    CHECK(tw.tile_size == 10);
    CHECK(tw.origin == Vec2{5, 7});
    CHECK(tw.at({0, 0}) == TileShape::HalfSlopeUpLeftLow);
    CHECK(tw.flags(0, 0) == kTileWater);
    CHECK(tw.cost(0, 0) == kDefaultTileCost); // the solid below no longer counts
    CHECK(tw.at({1, 0}) == TileShape::Empty);
    CHECK(tw.flags(1, 0) == kTileHazard);
    CHECK(tw.cost(1, 0) == 30);
    CHECK(tw.cost(2, 0) == 0);
    CHECK(tw.cost(0, 1) == 15);
    CHECK(tw.cost(2, 1) == kDefaultTileCost); // no tile
    CHECK(tw.at({2, 1}) == TileShape::Empty); // the art layer never collides
    const std::uint32_t v = tw.version;
    fill_tile_world(m, m.layers, sets, {5, 7}, tw);
    CHECK(tw.version == v + 1);
    // One layer's costs alone.
    Grid<std::uint16_t> cost;
    build_cost_grid(m, m.layers, sets, StrId{}, cost); // no layer by that name: defaults
    CHECK(cost.width() == 3);
    CHECK(cost(0, 0) == kDefaultTileCost);
    build_cost_grid(m, m.layers, sets, "base"_id, cost);
    CHECK(cost(0, 0) == 0); // solid
    CHECK(cost(1, 0) == 30);
    CHECK(cost(2, 0) == 0);
    CHECK(cost(1, 1) == kDefaultTileCost);
    build_cost_grid(m, m.layers, sets, "art"_id,
                    cost); // a named layer counts even without collision
    CHECK(cost(2, 1) == 0);
}

TEST_CASE("property helpers read text values and fall back on bad ones") {
    const std::vector<std::pair<StrId, std::string>> props = {
        {"n"_id, "42"}, {"neg"_id, "-7"},  {"f"_id, "1.25"},          {"b1"_id, "true"},
        {"b0"_id, "0"}, {"bad"_id, "12x"}, {"big"_id, "99999999999"}, {"word"_id, "maybe"}};
    CHECK(prop_int(props, "n"_id) == 42);
    CHECK(prop_int(props, "neg"_id) == -7);
    CHECK(prop_int(props, "bad"_id, 5) == 5);
    CHECK(prop_int(props, "big"_id, 5) == 5);
    CHECK(prop_float(props, "f"_id) == doctest::Approx(1.25f));
    CHECK(prop_float(props, "bad"_id, 2) == doctest::Approx(2.0f));
    CHECK(prop_bool(props, "b1"_id));
    CHECK_FALSE(prop_bool(props, "b0"_id, true));
    CHECK(prop_bool(props, "word"_id, true));
    CHECK(prop_or(props, "missing"_id, "x") == "x");
    CHECK(prop_or(props, "n"_id) == "42");
}

// ---- TilemapInstance ops ------------------------------------------------------------------------

namespace {

TilemapInstance sample_instance() {
    TilemapInstance inst;
    inst.version = 7;
    inst.asset_version = 3;
    inst.objects_spawned = true;
    TileLayer a;
    a.name = intern("ground");
    a.collision = true;
    a.render_layer = 17;
    a.opacity = 0.75f;
    a.parallax = {0.5f, 0.25f};
    a.tiles.resize(5, 3, 1);
    a.tiles(4, 2) = kTileFlipH | 9;
    TileLayer b;
    b.name = intern("deco");
    b.visible = false;
    b.tiles.resize(5, 3, 0);
    inst.layers = {a, b};
    return inst;
}

std::uint64_t hash_of(const TilemapInstance &m) {
    Hasher h;
    lam_hash(m, h);
    return h.value();
}

} // namespace

TEST_CASE("tilemap instance: saves round-trip run-length layers, hashes cover tiles") {
    const TilemapInstance inst = sample_instance();
    DataWriter out;
    lam_write(inst, out, SerializeContext{});
    const Result<std::string> json = out.finish();
    REQUIRE(json.ok());
    CHECK(json->find("\"runs\"") != std::string::npos);
    CHECK(json->size() < 600); // 30 cells in a handful of runs
    Result<DataDocument> doc = DataDocument::parse(*json, "save.json");
    REQUIRE(doc.ok());
    TilemapInstance back;
    DataReader root = doc->root();
    REQUIRE(lam_read(back, root, SerializeContext{}).ok());
    REQUIRE(doc->finish().ok());
    REQUIRE(back.layers.size() == 2);
    CHECK(back.version == 7);
    CHECK(back.objects_spawned);
    CHECK(back.asset_version == 0); // process-local: re-stamped by the restore hook
    CHECK(back.layers[0].name == "ground"_id);
    CHECK(back.layers[0].collision);
    CHECK(back.layers[0].render_layer == 17);
    CHECK(back.layers[0].opacity == doctest::Approx(0.75f));
    CHECK(back.layers[0].parallax == Vec2{0.5f, 0.25f});
    CHECK(back.layers[0].tiles == inst.layers[0].tiles);
    CHECK_FALSE(back.layers[1].visible);
    CHECK(back.layers[1].tiles == inst.layers[1].tiles);
    CHECK(hash_of(back) == hash_of(inst));
    TilemapInstance edited = inst;
    edited.layers[1].tiles(2, 2) = 4;
    CHECK(hash_of(edited) != hash_of(inst));
    edited = inst;
    edited.asset_version = 99; // not hashed
    CHECK(hash_of(edited) == hash_of(inst));
    CHECK(tile_at(inst, "ground"_id, {4, 2}) == (kTileFlipH | 9u));
    CHECK(tile_at(inst, "ground"_id, {5, 2}) == 0);
    CHECK(tile_at(inst, "nope"_id, {0, 0}) == 0);
}

TEST_CASE("tilemap instance: corrupt saves are errors, never partial grids") {
    const auto read = [](std::string_view json) {
        Result<DataDocument> doc = DataDocument::parse(json, "save.json");
        REQUIRE(doc.ok());
        TilemapInstance m;
        DataReader root = doc->root();
        const Status s = lam_read(m, root, SerializeContext{});
        return s.ok() && doc->finish().ok();
    };
    const char *layer =
        R"({"name": "a", "render_layer": 16, "collision": true, "visible": true, "opacity": 1,
                           "parallax": [1, 1], "tiles": %})";
    const auto with_tiles = [&](std::string_view tiles) {
        std::string l = layer;
        l.replace(l.find('%'), 1, tiles);
        return std::string(R"({"map": null, "version": 1, "objects_spawned": false, "layers": [)") +
               l + "]}";
    };
    CHECK(read(with_tiles(R"({"w": 2, "h": 2, "runs": [5, 4]})")));
    CHECK_FALSE(read(with_tiles(R"({"w": 2, "h": 2, "runs": [5, 3]})")));         // too few cells
    CHECK_FALSE(read(with_tiles(R"({"w": 2, "h": 2, "runs": [5, 5]})")));         // too many
    CHECK_FALSE(read(with_tiles(R"({"w": 2, "h": 2, "runs": [5]})")));            // odd
    CHECK_FALSE(read(with_tiles(R"({"w": 2, "h": 2, "runs": [-1, 4]})")));        // negative GID
    CHECK_FALSE(read(with_tiles(R"({"w": 65536, "h": 65536, "runs": [0, 1]})"))); // absurd size
    CHECK_FALSE(read(with_tiles(R"({"w": 2, "h": 2, "runs": [5, 4], "extra": 1})")));
}

// ---- the documented native format, Tiled backgroundcolor ----------------------------------------

namespace {

// The "complete example" from the tilemap.h header comment, with the comment markers stripped.
std::string header_example() {
    std::ifstream in(LAM_TILEMAP_HEADER);
    REQUIRE_MESSAGE(in.good(), "cannot open " LAM_TILEMAP_HEADER);
    std::string line;
    std::string out;
    bool inside = false;
    while (std::getline(in, line)) {
        if (!inside) {
            inside = line.find("A complete example") != std::string::npos;
            continue;
        }
        if (line.rfind("// Keys", 0) == 0) break;
        REQUIRE(line.rfind("//   ", 0) == 0);
        out += line.substr(5);
        out += '\n';
    }
    return out;
}

} // namespace

TEST_CASE("native map: the format example in tilemap.h parses exactly as documented") {
    const std::string text = header_example();
    REQUIRE(text.find("\"rows\"") != std::string::npos);
    Result<TilemapAsset> r = parse_native_map(text, "levels/example.map.json");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    const TilemapAsset &m = *r;
    CHECK(m.width == 7);
    CHECK(m.height == 4);
    CHECK(m.tile_size == 16);
    // Tilesets: the two named ones (left for the loader), then the generated terrain set.
    REQUIRE(m.tilesets.size() == 3);
    CHECK(m.tilesets[0].source == "tiles.tileset.json");
    CHECK(m.tilesets[0].first_gid == 1);
    CHECK(m.tilesets[1].source == "deco.tsj");
    CHECK(m.tilesets[1].first_gid == 65);
    CHECK(m.tilesets[2].first_gid == (1u << 24));
    CHECK(m.tilesets[2].embedded == 0);
    // Terrain values: legend order first, then terrains not named by the legend.
    REQUIRE(m.terrains.size() == 3);
    CHECK(m.terrains[0] == "wall"_id);
    CHECK(m.terrains[1] == "grass"_id);
    CHECK(m.terrains[2] == "water"_id);
    CHECK(m.terrain(0, 0) == 1);
    CHECK(m.terrain(3, 1) == 2); // 'T' is grass
    CHECK(m.terrain(2, 2) == 3);
    // Layers: hidden collision "terrain", visual "ground", then "deco".
    REQUIRE(m.layers.size() == 3);
    CHECK(m.layers[0].name == "terrain"_id);
    CHECK(m.layers[0].collision);
    CHECK_FALSE(m.layers[0].visible);
    CHECK(m.layers[0].tiles(0, 0) == (1u << 24));
    CHECK(m.layers[0].tiles(2, 2) == (1u << 24) + 2);
    CHECK(m.layers[1].name == "ground"_id);
    CHECK_FALSE(m.layers[1].collision);
    CHECK(m.layers[1].tiles(0, 0) == 3);
    CHECK(m.layers[1].tiles(1, 1) == 1);
    CHECK(m.layers[1].tiles(3, 1) == 2); // the legend's tile beats the terrain's
    CHECK(m.layers[1].tiles(2, 2) == 9);
    CHECK(m.layers[2].name == "deco"_id);
    CHECK_FALSE(m.layers[2].collision);
    CHECK(m.layers[2].render_layer == 20);
    CHECK(m.layers[2].tiles(5, 1) == 66);
    CHECK(m.layers[2].tiles(1, 2) == 65);
    // Objects: the legend's tree first (rows are read first), then "objects", in cells * 16 px.
    REQUIRE(m.objects.size() == 3);
    CHECK(m.objects[0].type == "tree"_id);
    CHECK(m.objects[0].rect == Rect{48, 16, 16, 16});
    CHECK(m.objects[1].type == "hero"_id);
    CHECK(m.objects[1].name == "start"_id);
    CHECK(m.objects[1].rect == Rect{16, 16, 16, 16});
    CHECK(m.objects[2].type == "door"_id);
    CHECK(prop_or(m.objects[2].props, "to"_id) == "level2");
    CHECK(prop_bool(m.objects[2].props, "locked"_id));
    CHECK(prop_or(m.properties, "music"_id) == "day");
    CHECK(prop_int(m.properties, "par_time"_id) == 90);
    // Collision and costs come from the terrains.
    const TileWorld tw = world_of(m);
    CHECK(tw.at({0, 0}) == TileShape::Solid);
    CHECK(tw.cost(0, 0) == 0);
    CHECK(tw.opaque({0, 0}));
    CHECK(tw.at({1, 1}) == TileShape::Empty);
    CHECK(tw.cost(1, 1) == kDefaultTileCost);
    CHECK(tw.cost(2, 2) == 30);
    CHECK(tw.flags(2, 2) == kTileWater);
}

TEST_CASE("native map: common mistakes are errors that say what to write instead") {
    // Apple Trail's first orchard.map.json, abridged: numbers in the legend, rows inside a layer,
    // an object rect in pixels.
    const std::string orchard = R"({"width": 4, "height": 2, "tile_size": 16,
      "legend": {".": 0, "#": 1},
      "layers": [{"name": "walls", "collision": true, "rows": ["####", "#..#"]}],
      "objects": [{"type": "start", "name": "start", "rect": [16, 16, 16, 16]}]})";
    CHECK(error_of(orchard).find("not numbers") != std::string::npos);
    CHECK(error_of(R"({"width": 4, "height": 2,
                       "layers": [{"name": "walls", "rows": ["####", "#..#"]}]})")
              .find("top-level \"rows\"") != std::string::npos);
    CHECK(error_of(R"({"legend": {".": "g"}, "rows": [".."],
                       "objects": [{"type": "start", "rect": [16, 0, 16, 16]}]})")
              .find("not \"rect\"") != std::string::npos);
    CHECK(error_of(R"({"legend": {".": "g"}, "rows": [".."],
                       "objects": [{"type": "start", "x": 16, "y": 0}]})")
              .find("cells, not pixels") != std::string::npos);
    CHECK(error_of(R"({"legend": {".": "g"}, "rows": [".."], "width": 3, "height": 1})")
              .find("differ from the rows") != std::string::npos);
    // The corrected map, as JSON and as plain ASCII text, gives the same collision.
    const std::string fixed = R"({"tile_size": 16,
      "terrains": {"wall": {"shape": "solid", "flags": ["opaque"], "cost": 0}},
      "legend": {"#": "wall", ".": "grass"},
      "rows": ["####", "#..#", "####"],
      "objects": [{"type": "start", "x": 1, "y": 1}]})";
    Result<TilemapAsset> json = parse_native_map(fixed, "orchard.map.json");
    REQUIRE_MESSAGE(json.ok(), (json.ok() ? std::string() : json.error().message));
    Result<TilemapAsset> ascii = parse_native_map("####\n#..#\n####\n", "orchard.txt");
    REQUIRE(ascii.ok());
    const TileWorld a = world_of(*json);
    const TileWorld b = world_of(*ascii);
    CHECK(a.shapes == b.shapes);
    CHECK(a.flags == b.flags);
    CHECK(a.cost == b.cost);
    REQUIRE(json->objects.size() == 1);
    CHECK(json->objects[0].rect == Rect{16, 16, 16, 16});
}

TEST_CASE("tiled: backgroundcolor is copied into the map properties unless a property has it") {
    const char *base = R"({"type": "map", "tiledversion": "1.10.2", "orientation": "orthogonal",
      "width": 1, "height": 1, "tilewidth": 16, "tileheight": 16, "infinite": false,
      "backgroundcolor": "#336699", "layers": [], "tilesets": [] PROPS})";
    std::string plain = base;
    plain.replace(plain.find("PROPS"), 5, "");
    Result<TilemapAsset> r = parse_tiled_json(plain, "bg.tmj");
    REQUIRE(r.ok());
    CHECK(prop_or(r->properties, "backgroundcolor"_id) == "#336699");
    std::string custom = base;
    custom.replace(custom.find("PROPS"), 5,
                   R"(, "properties": [{"name": "backgroundcolor", "type": "string", "value": "dusk"}])");
    r = parse_tiled_json(custom, "bg.tmj");
    REQUIRE(r.ok());
    CHECK(r->properties.size() == 1);
    CHECK(prop_or(r->properties, "backgroundcolor"_id) == "dusk");
}

// ---- Regressions --------------------------------------------------------------------------------

TEST_CASE("native map: maps over 2^26 cells are refused before their grids are sized") {
    // 8193 x 8193 cells from 16 KB of text: a wall in the first and the last row, empty between.
    std::string ascii(8193, '#');
    ascii += std::string(8192, '\n');
    ascii += '#';
    const Result<TilemapAsset> a = parse_native_map(ascii, "huge.txt");
    REQUIRE_FALSE(a.ok());
    CHECK(a.error().code == ErrorCode::Parse);
    CHECK(a.error().message.find("huge.txt") != std::string::npos);
    CHECK(a.error().message.find("too large") != std::string::npos);
    // JSON width/height: the same cap.
    CHECK(error_of(R"({"width": 8193, "height": 8193})").find("too large") != std::string::npos);
    // Long and thin is fine.
    CHECK(
        parse_native_map(std::string(4000, '.') + "\n" + std::string(4000, '#'), "thin.txt").ok());
}

TEST_CASE("native map: generated GIDs never reach the flip bits") {
    const auto map = [](std::string_view extra) {
        return std::string(R"({"legend": {"#": "wall", ".": "floor"}, "rows": ["#.", ".#"])") +
               std::string(extra) + "}";
    };
    // Two terrains from the last GID: the second would be 0x20000000 = kTileFlipD with id 0.
    CHECK(error_of(map(R"(, "terrain_first_gid": 536870911)")).find("GID range") !=
          std::string::npos);
    const Result<TilemapAsset> edge =
        parse_native_map(map(R"(, "terrain_first_gid": 536870910)"), "edge.map.json");
    REQUIRE_MESSAGE(edge.ok(), (edge.ok() ? std::string() : edge.error().message));
    const TileLayer *terrain = find_layer(edge->layers, "terrain"_id);
    REQUIRE(terrain != nullptr);
    CHECK(terrain->tiles(0, 0) == 536870910u);
    CHECK(terrain->tiles(1, 0) == kTileIdMask);
    // Autotile rules: blob47 spans 47 * variants GIDs from "first", edge4 16 *, plain 1 *.
    const auto rule = [&](std::string_view mode, int variants, std::uint32_t first) {
        return error_of(map(R"(, "autotile": {"seed": 0, "rules": [{"terrain": "wall", "mode": ")" +
                            std::string(mode) + R"(", "variants": )" + std::to_string(variants) +
                            R"(, "first": )" + std::to_string(first) + "}]}"));
    };
    CHECK(rule("blob47", 255, kTileIdMask - 47 * 255 + 1) == "");
    CHECK(rule("blob47", 255, kTileIdMask - 47 * 255 + 2).find("GID range") != std::string::npos);
    CHECK(rule("edge4", 1, kTileIdMask - 16 + 1) == "");
    CHECK(rule("edge4", 1, kTileIdMask - 16 + 2).find("GID range") != std::string::npos);
    CHECK(rule("plain", 3, kTileIdMask - 3 + 1) == "");
    CHECK(rule("plain", 3, kTileIdMask - 3 + 2).find("/autotile/rules/0") != std::string::npos);
}

TEST_CASE("property helpers: prop_float reads '.' whatever the process locale") {
    const char *current = std::setlocale(LC_NUMERIC, nullptr);
    const std::string previous = current != nullptr ? current : "C";
    bool comma = false;
    for (const char *name : {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "fr_FR", "de_DE.ISO8859-1"}) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr &&
            std::string_view(std::localeconv()->decimal_point) == ",") {
            comma = true;
            break;
        }
    }
    const std::vector<std::pair<StrId, std::string>> props = {{"speed"_id, "2.5"},
                                                              {"comma"_id, "2,5"}};
    const float speed = prop_float(props, "speed"_id, 1);
    const float with_comma = prop_float(props, "comma"_id, 1);
    std::setlocale(LC_NUMERIC, previous.c_str());
    if (!comma) MESSAGE("no comma-decimal locale installed: checked under the C locale only");
    CHECK(speed == doctest::Approx(2.5f));
    CHECK(with_comma == doctest::Approx(1.0f)); // not a number with a '.' separator
}
