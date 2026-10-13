// TilemapPlugin: loaders, tilemap_sync, edits and undo, path requests and draw_tiles.
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/data.h>
#include <lam/core/hash.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/physics_plugin.h>
#include <lam/render/atlas.h>
#include <lam/render/camera.h>
#include <lam/render/render_queue.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>
#include <lam/tilemap/fov.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/tilemap/grid_path.h>
#include <lam/tilemap/path_follow.h>
#include <lam/tilemap/tilemap.h>
#include <lam/tilemap/tilemap_plugin.h>

using namespace lam;
using namespace lam::literals;

namespace {

// ---- a valid RGBA PNG (stored deflate blocks), so any decoder accepts it ------------------------

std::uint32_t crc32(std::string_view bytes) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (const char ch : bytes) {
        c ^= static_cast<std::uint8_t>(ch);
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void put_be32(std::string &out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
        out += static_cast<char>((v >> s) & 0xFFu);
}

void chunk(std::string &png, std::string_view type, std::string_view data) {
    put_be32(png, static_cast<std::uint32_t>(data.size()));
    std::string body(type);
    body += data;
    png += body;
    put_be32(png, crc32(body));
}

std::string make_png(int w, int h) {
    std::string raw;
    for (int y = 0; y < h; ++y) {
        raw += '\0'; // filter: none
        for (int x = 0; x < w; ++x)
            raw += std::string{static_cast<char>(x * 8), static_cast<char>(y * 8), '\x40', '\xFF'};
    }
    std::string z = "\x78\x01";
    std::size_t at = 0;
    do {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        z += static_cast<char>(at + n == raw.size() ? 1 : 0);
        z += static_cast<char>(n & 0xFF);
        z += static_cast<char>(n >> 8);
        z += static_cast<char>(~n & 0xFF);
        z += static_cast<char>((~n >> 8) & 0xFF);
        z.append(raw, at, n);
        at += n;
    } while (at < raw.size());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const char ch : raw) {
        a = (a + static_cast<std::uint8_t>(ch)) % 65521u;
        b = (b + a) % 65521u;
    }
    put_be32(z, (b << 16) | a);
    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    put_be32(ihdr, static_cast<std::uint32_t>(w));
    put_be32(ihdr, static_cast<std::uint32_t>(h));
    ihdr += std::string{'\x08', '\x06', '\0', '\0', '\0'}; // 8-bit RGBA
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    return png;
}

// ---- the test level -----------------------------------------------------------------------------

// 6 x 4 cells of 16 px. Embedded "ground" set (GID 1..8): local 1 is a wall, local 4 animates
// through 4, 5, 6 every 100 ms. External "deco" set (GID 9..10): local 0 is a hazard.
constexpr const char *kForest = R"({
  "type": "map", "tiledversion": "1.10.2", "orientation": "orthogonal", "infinite": false,
  "width": 6, "height": 4, "tilewidth": 16, "tileheight": 16,
  "tilesets": [
    {"firstgid": 1, "name": "ground", "tilewidth": 16, "tileheight": 16, "tilecount": 8, "columns": 4,
     "image": "tiles.png", "imagewidth": 64, "imageheight": 32,
     "tiles": [{"id": 1, "properties": [{"name": "shape", "type": "string", "value": "solid"},
                                        {"name": "opaque", "type": "bool", "value": true},
                                        {"name": "cost", "type": "int", "value": 0}]},
               {"id": 4, "animation": [{"tileid": 4, "duration": 100}, {"tileid": 5, "duration": 100},
                                       {"tileid": 6, "duration": 100}]}]},
    {"firstgid": 9, "source": "deco.tsj"}
  ],
  "layers": [
    {"name": "ground", "type": "tilelayer", "width": 6, "height": 4, "opacity": 1, "visible": true,
     "data": [2, 2, 2, 2, 2, 2,
              2, 1, 1, 5, 1, 2,
              2, 1, 9, 1, 2147483649, 2,
              2, 2, 2, 2, 2, 2]},
    {"name": "over", "type": "tilelayer", "width": 6, "height": 4, "opacity": 0.5, "visible": true,
     "properties": [{"name": "collision", "type": "bool", "value": false},
                    {"name": "render_layer", "type": "int", "value": 70}],
     "data": [0, 0, 0, 0, 0, 0,  0, 3, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0]},
    {"name": "things", "type": "objectgroup", "objects": [
      {"id": 1, "name": "start", "type": "hero", "x": 16, "y": 16, "width": 16, "height": 16},
      {"id": 2, "name": "", "class": "coin", "gid": 5, "x": 48, "y": 32, "width": 16, "height": 16},
      {"id": 3, "name": "readme", "type": "note", "x": 0, "y": 0, "width": 0, "height": 0}]}
  ]
})";

constexpr const char *kDeco =
    R"({"type": "tileset", "name": "deco", "tilewidth": 16, "tileheight": 16,
  "tilecount": 2, "columns": 2, "image": "deco.png", "imagewidth": 32, "imageheight": 16,
  "tiles": [{"id": 0, "properties": [{"name": "hazard", "type": "bool", "value": true}]}]})";

constexpr const char *kPrefabs = R"({"prefabs": {
  "hero": {"components": {"Transform": {}, "GridPosition": {}}},
  "coin": {"components": {"Transform": {}}}
}})";

struct Level {
    test::TempDir dir;
    test::HeadlessApp h;
    explicit Level(TilemapConfig config = {}) {
        test::write_test_file(dir, "maps/forest.tmj", kForest);
        test::write_test_file(dir, "maps/deco.tsj", kDeco);
        test::write_test_file(dir, "maps/tiles.png", make_png(64, 32));
        test::write_test_file(dir, "maps/deco.png", make_png(32, 16));
        AssetsConfig ac;
        ac.root = dir.path();
        h.add<AssetsPlugin>(ac);
        h.add<PhysicsPlugin>();
        h.add<TilemapPlugin>(config);
        REQUIRE(h.app()
                    .res<PrefabLibrary>()
                    .load_text(kPrefabs, "prefabs.json", h.app().serialize_context())
                    .ok());
    }
    App &app() { return h.app(); }
    World &world() { return h.app().main_world(); }
    AssetServer &assets() { return h.app().res<AssetServer>(); }
    Entity add_map(std::string_view path, Vec2 origin = {100, 50}, bool collision = true) {
        const AssetId<TilemapAsset> id = assets().load<TilemapAsset>(path);
        const Entity e = world().create();
        world().add<Transform>(e, Transform{.position = origin});
        world().add<Tilemap>(e, Tilemap{.map = id, .build_collision = collision});
        return e;
    }
};

// The schedule line of a system: "<stage> <scope> <order>  <name>  [plugin]".
bool scheduled(const App &app, std::string_view name, std::string_view stage, int order) {
    const std::string text = app.schedule_text();
    const std::string want =
        std::format("{:<11} {:<8} {:>6}  {}  [lam.tilemap]", stage, "PerWorld", order, name);
    return text.find(want) != std::string::npos;
}

int count_named(World &w, StrId name) {
    int n = 0;
    for (auto [e, nm] : w.view<Name>()) {
        (void)e;
        n += nm.id == name ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("tilemap plugin: systems at their stage, scope and order; world resources") {
    Level level;
    const App &app = level.app();
    CHECK(scheduled(app, "tilemap_sync", "FrameBegin", -1000));
    CHECK(scheduled(app, "tilemap_sync_tick", "TickBegin", -1900));
    CHECK(scheduled(app, "grid_index_rebuild", "Simulate", -1500));
    CHECK(scheduled(app, "serve_path_requests", "Simulate", -1100));
    CHECK(scheduled(app, "grid_movers", "Animate", -1200));
    CHECK(scheduled(app, "animated_tiles", "Animate", -1100));
    CHECK(scheduled(app, "draw_tiles", "Render", -1000));
    World &w = level.world();
    CHECK(w.resources().has<TileWorld>());
    CHECK(w.resources().has<GridIndex>());
    REQUIRE(w.resources().has<PathService>());
    CHECK(w.resources().get<PathService>().finder.capacity() == 65536);
    CHECK(w.resources().get<PathService>().searches_per_tick == 8);
    World &other = level.app().create_world("other");
    CHECK(other.resources().has<TileWorld>());
    CHECK(other.resources().has<GridIndex>());
    CHECK(other.resources().has<PathService>());
}

TEST_CASE("tilemap plugin: a Tiled map loads, fills TileWorld and spawns its objects once") {
    Level level;
    const Entity map = level.add_map("maps/forest.tmj");
    AssetServer &assets = level.assets();
    const AssetId<TilemapAsset> id = level.world().get<Tilemap>(map).map;
    REQUIRE_MESSAGE(
        assets.state(id) == LoadState::Ready,
        (assets.error(id) != nullptr ? assets.error(id)->message : std::string("not ready")));
    const TilemapAsset &asset = *assets.get(id);
    REQUIRE(asset.tilesets.size() == 2);
    const Tileset *sets[kMaxMapTilesets] = {};
    REQUIRE(map_tilesets(assets, asset, sets) == 2);
    REQUIRE(sets[0] != nullptr);
    REQUIRE(sets[1] != nullptr); // the external .tsj, loaded as a dependency
    CHECK(sets[0]->regions.size() == 8);
    CHECK(sets[0]->region(5).source == Rect{16, 16, 16, 16});
    CHECK(sets[0]->texture);
    CHECK(sets[0]->pending_image.width == 0); // cleared at finalize
    CHECK(assets.get(sets[0]->texture) != nullptr);
    CHECK(sets[1]->prop(0).flags == kTileHazard);
    CHECK(sets[1]->regions.size() == 2);

    World &w = level.world();
    level.h.frames(1);
    const TilemapInstance *inst = w.try_get<TilemapInstance>(map);
    REQUIRE(inst != nullptr);
    CHECK(inst->map == id);
    REQUIRE(inst->layers.size() == 2);
    CHECK(inst->layers[0].tiles == asset.layers[0].tiles);
    CHECK(inst->objects_spawned);
    const TileWorld &tw = w.resources().get<TileWorld>();
    CHECK(tw.shapes.width() == 6);
    CHECK(tw.shapes.height() == 4);
    CHECK(tw.tile_size == 16);
    CHECK(tw.origin == Vec2{100, 50});
    CHECK(tw.at({0, 0}) == TileShape::Solid);
    CHECK(tw.at({1, 1}) == TileShape::Empty);
    CHECK(tw.cost(0, 0) == 0);
    CHECK(tw.cost(1, 1) == kDefaultTileCost);
    CHECK(tw.flags(2, 2) == kTileHazard);
    CHECK(tw.opaque({5, 3}));
    // Objects with a prefab became entities at the map origin + their rect centre.
    REQUIRE(count_named(w, "start"_id) == 1);
    Entity hero;
    for (auto [e, nm] : w.view<Name>()) {
        if (nm.id == "start"_id) hero = e;
    }
    CHECK(w.get<Transform>(hero).position == Vec2{124, 74});
    REQUIRE(w.has<GridPosition>(hero));
    CHECK(w.get<GridPosition>(hero).cell == IVec2{1, 1});
    int coins = 0;
    for (auto [e, t] : w.view<Transform>(Exclude<Tilemap, Name>{})) {
        if (t.position == Vec2{156, 74})
            ++coins; // tile object: moved up from its bottom-left anchor
        (void)e;
    }
    CHECK(coins == 1);
    // More frames spawn nothing more.
    const std::size_t entities = w.entity_count();
    level.h.frames(3);
    CHECK(w.entity_count() == entities);
    CHECK(count_named(w, "start"_id) == 1);
}

TEST_CASE("tilemap plugin: set_tile edits the instance and its TileWorld cell, never the asset") {
    Level level;
    const Entity map = level.add_map("maps/forest.tmj");
    level.h.frames(1);
    World &w = level.world();
    TileWorld &tw = w.resources().get<TileWorld>();
    const TilemapAsset &asset = *level.assets().get(w.get<Tilemap>(map).map);
    const std::uint32_t version = w.get<TilemapInstance>(map).version;
    const std::uint64_t hash = w.state_hash();
    const std::uint32_t tw_before = tw.version;
    set_tile(w, map, "ground"_id, {1, 1}, 2);
    CHECK(tw.version == tw_before + 1);                          // shape, flags and cost: one bump
    CHECK(tile_at(w.get<TilemapInstance>(map), "ground"_id, {1, 1}) == 2);
    CHECK(asset.layers[0].tiles(1, 1) == 1);
    CHECK(w.get<TilemapInstance>(map).version == version + 1);
    CHECK(tw.at({1, 1}) == TileShape::Solid);
    CHECK(tw.cost(1, 1) == 0);
    CHECK(tw.opaque({1, 1}));
    CHECK(w.state_hash() != hash);
    // A non-collision layer: the instance changes, TileWorld does not.
    const std::uint32_t tw_version = tw.version;
    set_tile(w, map, "over"_id, {2, 2}, 2);
    CHECK(tile_at(w.get<TilemapInstance>(map), "over"_id, {2, 2}) == 2);
    CHECK(tw.version == tw_version);
    CHECK(tw.at({2, 2}) == TileShape::Empty);
    // No-ops: same value, outside, unknown layer, an entity without an instance.
    const std::uint32_t v2 = w.get<TilemapInstance>(map).version;
    set_tile(w, map, "ground"_id, {1, 1}, 2);
    set_tile(w, map, "ground"_id, {6, 0}, 2);
    set_tile(w, map, "nope"_id, {0, 0}, 2);
    set_tile(w, w.create(), "ground"_id, {0, 0}, 2);
    CHECK(w.get<TilemapInstance>(map).version == v2);
    // Clearing the wall gives the cell back; the edit survives later frames.
    set_tile(w, map, "ground"_id, {1, 1}, 0);
    CHECK(tw.at({1, 1}) == TileShape::Empty);
    CHECK(tw.cost(1, 1) == kDefaultTileCost);
    set_tile(w, map, "ground"_id, {3, 2}, 2);
    level.h.frames(2);
    CHECK(tw.at({3, 2}) == TileShape::Solid);
    CHECK_NO_ALLOC(set_tile(w, map, "ground"_id, {4, 1}, 2);
                   set_tile(w, map, "ground"_id, {4, 1}, 1));
}

TEST_CASE("tilemap plugin: undo restores tiles, refills TileWorld and rebuilds GridIndex") {
    Level level;
    const Entity map = level.add_map("maps/forest.tmj");
    level.h.frames(1);
    World &w = level.world();
    const TileWorld &tw = w.resources().get<TileWorld>();
    Entity hero;
    for (auto [e, nm] : w.view<Name>()) {
        if (nm.id == "start"_id) hero = e;
    }
    REQUIRE(w.alive(hero));
    level.h.ticks(1);
    WorldSnapshot snap;
    w.snapshot_into(snap);
    const std::uint64_t hash = w.state_hash();
    set_tile(w, map, "ground"_id, {2, 1}, 2);
    CHECK(try_step(w, hero, {1, 0}) == StepResult::BlockedTile);
    CHECK(try_step(w, hero, {0, 1}) == StepResult::Moved);
    CHECK(w.state_hash() != hash);
    w.restore(snap);
    CHECK(w.state_hash() == hash);
    CHECK(tile_at(w.get<TilemapInstance>(map), "ground"_id, {2, 1}) == 1);
    CHECK(tw.at({2, 1}) == TileShape::Empty); // refilled by the restore hook
    CHECK(w.get<GridPosition>(hero).cell == IVec2{1, 1});
    const GridIndex &index = w.resources().get<GridIndex>();
    REQUIRE(index.at({1, 1}).size() == 1);
    CHECK(index.at({1, 1})[0] == hero);
    CHECK(index.at({1, 2}).empty());
    CHECK(try_step(w, hero, {1, 0}) == StepResult::Moved);
    // Later frames keep the restored state (no second spawn of the map's objects).
    level.h.frames(2);
    CHECK(count_named(w, "start"_id) == 1);
    CHECK(w.get<TilemapInstance>(map).objects_spawned);
}

TEST_CASE("tilemap plugin: a Modified map replaces the instance (the asset wins over edits)") {
    Level level;
    const Entity map = level.add_map("maps/forest.tmj");
    level.h.frames(1);
    World &w = level.world();
    set_tile(w, map, "ground"_id, {4, 1}, 2);
    const std::uint32_t version = w.get<TilemapInstance>(map).version;
    AssetServer &assets = level.assets();
    const AssetId<TilemapAsset> id = w.get<Tilemap>(map).map;
    TilemapAsset *asset = assets.get_mut(id);
    REQUIRE(asset != nullptr);
    asset->layers[0].tiles(2, 1) = 2;
    assets.mark_modified(id);
    level.h.frames(1);
    const TilemapInstance &inst = w.get<TilemapInstance>(map);
    CHECK(inst.version == version + 1);
    CHECK(inst.asset_version == assets.version(id));
    CHECK(tile_at(inst, "ground"_id, {2, 1}) == 2);
    CHECK(tile_at(inst, "ground"_id, {4, 1}) == 1); // the edit is gone
    const TileWorld &tw = w.resources().get<TileWorld>();
    CHECK(tw.at({2, 1}) == TileShape::Solid);
    CHECK(tw.at({4, 1}) == TileShape::Empty);
    CHECK(count_named(w, "start"_id) == 1); // objects are not spawned again
}

TEST_CASE(
    "tilemap plugin: native JSON and ASCII maps load; the first collision map owns TileWorld") {
    Level level;
    test::write_test_file(level.dir, "maps/room.map.json", R"({
      "legend": {"#": "wall", ".": "floor"},
      "terrains": {"wall": {"shape": "solid", "flags": ["opaque"], "cost": 0}},
      "rows": ["#####", "#...#", "#####"]})");
    test::write_test_file(level.dir, "maps/level.txt", "####\n#..#\n#..#\n####\n");
    const Entity art = level.add_map("maps/level.txt", {0, 0}, false); // decoration only
    const Entity room = level.add_map("maps/room.map.json", {0, 0});
    AssetServer &assets = level.assets();
    REQUIRE(assets.state(level.world().get<Tilemap>(room).map) == LoadState::Ready);
    REQUIRE(assets.state(level.world().get<Tilemap>(art).map) == LoadState::Ready);
    level.h.frames(1);
    World &w = level.world();
    CHECK(w.has<TilemapInstance>(art));
    CHECK(w.has<TilemapInstance>(room));
    const TileWorld &tw = w.resources().get<TileWorld>();
    CHECK(tw.shapes.width() == 5); // the room, not the ASCII art
    CHECK(tw.at({0, 0}) == TileShape::Solid);
    CHECK(tw.at({2, 1}) == TileShape::Empty);
    // Removing the collision map hands TileWorld to the next one that wants it.
    w.get<Tilemap>(art).build_collision = true;
    w.destroy(room);
    level.h.frames(1);
    CHECK(tw.shapes.width() == 4);
    CHECK(tw.shapes.height() == 4);
    CHECK(tw.at({1, 1}) == TileShape::Empty);
    CHECK(tw.at({0, 1}) == TileShape::Solid);
    // Moving the map entity moves its collision.
    w.get<Transform>(art).position = {32, 64};
    level.h.frames(1);
    CHECK(tw.origin == Vec2{32, 64});
}

TEST_CASE("tilemap plugin: broken maps fail with a message naming the file") {
    Level level;
    std::string zipped = kForest;
    zipped.replace(zipped.find("\"opacity\": 0.5"), 14,
                   "\"opacity\": 0.5, \"compression\": \"zlib\"");
    test::write_test_file(level.dir, "maps/zipped.tmj", zipped);
    test::write_test_file(
        level.dir, "maps/noimage.tsj",
        R"({"tilewidth": 16, "tileheight": 16, "image": "missing.png", "imagewidth": 16, "imageheight": 16})");
    test::CaptureLog log;
    AssetServer &assets = level.assets();
    const AssetId<TilemapAsset> z = assets.load<TilemapAsset>("maps/zipped.tmj");
    CHECK(assets.state(z) == LoadState::Failed);
    REQUIRE(assets.error(z) != nullptr);
    CHECK(assets.error(z)->code == ErrorCode::Unsupported);
    CHECK(assets.error(z)->message.find("zipped.tmj") != std::string::npos);
    const AssetId<Tileset> t = assets.load<Tileset>("maps/noimage.tsj");
    CHECK(assets.state(t) == LoadState::Failed);
    // A map entity whose asset failed stays without an instance and leaves TileWorld alone.
    const Entity e = level.world().create();
    level.world().add<Tilemap>(e, Tilemap{.map = z});
    level.h.frames(2);
    CHECK_FALSE(level.world().has<TilemapInstance>(e));
}

TEST_CASE("tilemap plugin: tilesets pack into the AtlasSet when RenderPlugin provides one") {
    Level level;
    level.app().insert_resource<AtlasSet>(256, 1);
    const Entity map = level.add_map("maps/forest.tmj");
    const TilemapAsset &asset = *level.assets().get(level.world().get<Tilemap>(map).map);
    const Tileset *sets[kMaxMapTilesets] = {};
    map_tilesets(level.assets(), asset, sets);
    REQUIRE(sets[0] != nullptr);
    const std::span<const AssetId<Texture>> pages = level.app().res<AtlasSet>().pages("default"_id);
    REQUIRE_FALSE(pages.empty());
    CHECK(sets[0]->texture == pages[0]);
    const TextureRegion r0 = sets[0]->region(0);
    const TextureRegion r5 = sets[0]->region(5);
    CHECK(r5.source.x - r0.source.x == 16); // the sheet's layout survives packing
    CHECK(r5.source.y - r0.source.y == 16);
    CHECK(r0.size == Vec2{16, 16});
}

// ---- path requests -----------------------------------------------------------------------------

namespace {

// 20 x 10, a wall at x = 10 with a gap at the bottom row.
void start_paths(Ctx &ctx) {
    World &w = ctx.world();
    TileWorld &tw = w.resources().get<TileWorld>();
    tw.resize(20, 10, 16, {0, 0});
    for (int y = 0; y < 9; ++y) {
        tw.set({10, y}, TileShape::Solid, kTileOpaque);
        tw.cost(10, y) = 0;
    }
    for (int i = 0; i < 10; ++i) {
        const Entity e = w.create();
        w.add<GridPosition>(e, GridPosition{{2, i}});
        w.add<PathRequest>(e, PathRequest{.goal = {17, 5}, .smoothing = PathSmoothing::Corners});
    }
}

// Every walker asks again each tick (a stress test of the budget and of allocations).
void repath(Ctx &ctx) {
    World &w = ctx.world();
    for (auto [e, gp] : w.view<GridPosition>()) {
        (void)gp;
        if (!w.has<PathRequest>(e)) {
            w.add<PathRequest>(e, PathRequest{.goal = {17, 5},
                                              .requested_tick = w.tick(),
                                              .smoothing = PathSmoothing::Corners});
        }
    }
}

} // namespace

TEST_CASE("tilemap plugin: path requests are served within the per-tick budget") {
    test::HeadlessApp h;
    h.add<AssetsPlugin>();
    h.add<PhysicsPlugin>();
    h.add<TilemapPlugin>(TilemapConfig{.path_searches_per_tick = 3, .max_path_cells = 4096});
    h.app().add_startup(&start_paths, "start_paths");
    h.ticks(1);
    World &w = h.app().main_world();
    CHECK(w.count<PathRoute>() == 3);
    CHECK(w.count<PathRequest>() == 7);
    for (auto [e, route] : w.view<PathRoute>()) {
        (void)e;
        CHECK(route.complete);
        CHECK(route.count > 0);
        CHECK(route.cells[route.count - 1] == IVec2{17, 5});
        CHECK(route.goal == IVec2{17, 5});
        CHECK(route.computed_tick == w.tick());
        CHECK(route.next == 0);
        // Corners: every leg is one of the 8 step directions.
        IVec2 at{2, 0};
        for (auto [e2, gp] : w.view<GridPosition>()) {
            if (e2 == e) at = gp.cell;
        }
        for (int i = 0; i < route.count; ++i) {
            const IVec2 d = route.cells[static_cast<std::size_t>(i)] - at;
            CHECK((d.x == 0 || d.y == 0 || std::abs(d.x) == std::abs(d.y)));
            at = route.cells[static_cast<std::size_t>(i)];
        }
    }
    h.ticks(3);
    CHECK(w.count<PathRoute>() == 10);
    CHECK(w.count<PathRequest>() == 0);
    // Starts: GridPosition, else the TileWorld cell under Transform; neither drops the request.
    const Entity walker = w.create();
    w.add<Transform>(walker, Transform{.position = {3 * 16 + 4, 4 * 16 + 9}});
    w.add<PathRequest>(walker, PathRequest{.goal = {3, 8}});
    const Entity lost = w.create();
    w.add<PathRequest>(lost, PathRequest{.goal = {3, 8}});
    h.ticks(1);
    REQUIRE(w.has<PathRoute>(walker));
    CHECK(w.get<PathRoute>(walker).complete);
    CHECK(w.get<PathRoute>(walker).count == 1); // in plain view (LineOfSight): the goal itself
    CHECK_FALSE(w.has<PathRequest>(lost));
    CHECK_FALSE(w.has<PathRoute>(lost));
    // Steady state: 10 walkers re-asking every tick allocate nothing.
    h.app().add_system({.stage = Stage::Simulate, .fn = &repath, .name = "repath", .order = -1200});
    h.ticks(120);
    CHECK_NO_ALLOC(h.ticks(600));
}

TEST_CASE("tilemap plugin: a failed path request leaves an empty route and warns once") {
    test::CaptureLog log;
    test::HeadlessApp h;
    h.add<AssetsPlugin>();
    h.add<PhysicsPlugin>();
    h.add<TilemapPlugin>(TilemapConfig{.path_searches_per_tick = 3, .max_path_cells = 100});
    h.app().add_startup(&start_paths, "start_paths"); // 200 cells > 100
    h.ticks(1);
    World &w = h.app().main_world();
    CHECK(w.count<PathRequest>() == 0); // errors are served (no budget needed)
    CHECK(w.count<PathRoute>() == 10);
    for (auto [e, route] : w.view<PathRoute>()) {
        (void)e;
        CHECK(route.count == 0);
        CHECK_FALSE(route.complete);
        CHECK(route.goal == IVec2{17, 5});
    }
    const std::string text = log.text();
    const std::size_t first = text.find("path request failed");
    REQUIRE(first != std::string::npos);
    CHECK(text.find("path request failed", first + 1) == std::string::npos); // once
    h.ticks(3);
    CHECK(log.text().find("path request failed", first + 1) == std::string::npos);
}

// ---- drawing ------------------------------------------------------------------------------------

namespace {

struct Drawn {
    std::vector<DrawQuad> tiles; // tile quads (render layers other than the fog's)
    std::vector<DrawQuad> fog;   // FogStyle::layer quads
};

Drawn collect(RenderQueue &q) {
    Drawn d;
    for (const DrawQuad &quad : q.quads()) {
        if (key_layer(quad.key) == kLayerForeground)
            d.fog.push_back(quad);
        else
            d.tiles.push_back(quad);
    }
    q.clear();
    return d;
}

const DrawQuad *quad_at(const std::vector<DrawQuad> &qs, Vec2 top_left) {
    for (const DrawQuad &q : qs) {
        if (q.v[0].pos == top_left) return &q;
    }
    return nullptr;
}

CameraView view_of(Rect visible) {
    CameraView v;
    v.valid = true;
    v.visible_world = visible;
    v.center = visible.center();
    return v;
}

} // namespace

TEST_CASE("tilemap plugin: draw_tiles pushes visible tiles with flips, opacity and animation") {
    Level level;
    level.add_map("maps/forest.tmj");
    RenderQueue &q = level.app().insert_resource<RenderQueue>();
    World &w = level.world();
    w.resources().insert<CameraView>(view_of(Rect{100, 50, 96, 64})); // the whole map
    level.h.ticks(1);
    Drawn d = collect(q);
    CHECK(d.tiles.size() == 25); // 24 ground cells + 1 "over" tile
    CHECK(d.fog.empty());
    // A plain wall at (0, 0): ground layer, opaque, unflipped UVs of local 1.
    const DrawQuad *wall = quad_at(d.tiles, {100, 50});
    REQUIRE(wall != nullptr);
    CHECK(key_layer(wall->key) == kLayerTiles);
    CHECK(wall->v[0].uv == Vec2{16, 0});
    CHECK(wall->v[2].uv == Vec2{32, 16});
    CHECK(wall->v[2].pos == Vec2{116, 66});
    CHECK(wall->v[0].color.a == 255);
    // The flipped tile at (4, 2): left and right UVs swap.
    const DrawQuad *flipped = quad_at(d.tiles, {164, 82});
    REQUIRE(flipped != nullptr);
    CHECK(flipped->v[0].uv == Vec2{16, 0});
    CHECK(flipped->v[1].uv == Vec2{0, 0});
    // The "over" layer: render layer 70, half opacity.
    int over = 0;
    for (const DrawQuad &quad : d.tiles) {
        if (key_layer(quad.key) != 70) continue;
        ++over;
        CHECK(quad.v[0].color.a == 128);
        CHECK(quad.v[0].pos == Vec2{116, 66});
    }
    CHECK(over == 1);
    // The animated tile at (3, 1) runs 4 -> 5 -> 6 every 100 ms (6 ticks at 60 Hz).
    const auto anim_uv_after = [&](int ticks) {
        if (ticks > 1) level.h.ticks(ticks - 1);
        q.clear();
        level.h.ticks(1);
        const Drawn now = collect(q);
        const DrawQuad *a = quad_at(now.tiles, {148, 66});
        REQUIRE(a != nullptr);
        return a->v[0].uv;
    };
    CHECK(anim_uv_after(4) == Vec2{0, 16});  // 5 ticks: still frame 0 (local 4)
    CHECK(anim_uv_after(1) == Vec2{16, 16}); // 6 ticks = 100 ms: local 5
    CHECK(anim_uv_after(6) == Vec2{32, 16}); // local 6
    CHECK(anim_uv_after(6) == Vec2{0, 16});  // and round again
    // Culling: a view over 3 x 2 cells draws only those (plus the over tile inside them).
    w.resources().get<CameraView>() = view_of(Rect{100, 50, 48, 32});
    level.h.ticks(1);
    d = collect(q);
    CHECK(d.tiles.size() == 7);
    // A view off the map draws nothing; an invalid view draws nothing.
    w.resources().get<CameraView>() = view_of(Rect{-500, -500, 100, 100});
    level.h.ticks(1);
    CHECK(collect(q).tiles.empty());
    w.resources().get<CameraView>() = view_of(Rect{100, 50, 96, 64});
    w.resources().get<CameraView>().valid = false;
    level.h.ticks(1);
    CHECK(collect(q).tiles.empty());
}

TEST_CASE(
    "tilemap plugin: parallax shifts a layer, hidden layers are skipped, fog covers the unseen") {
    Level level;
    const Entity map = level.add_map("maps/forest.tmj");
    RenderQueue &q = level.app().insert_resource<RenderQueue>();
    World &w = level.world();
    w.resources().insert<CameraView>(view_of(Rect{100, 50, 96, 64}));
    level.h.ticks(1);
    (void)collect(q);
    TilemapInstance &inst = w.get<TilemapInstance>(map);
    inst.layers[1].parallax = {0.5f, 1};
    inst.layers[0].visible = false;
    level.h.ticks(1);
    Drawn d = collect(q);
    REQUIRE(d.tiles.size() == 1);
    // centre (148, 82): the layer moves by centre * (1 - parallax) = 74 px right.
    CHECK(d.tiles[0].v[0].pos == Vec2{116 + 74, 66});
    inst.layers[0].visible = true;
    inst.layers[1].opacity = 0;
    level.h.ticks(1);
    CHECK(collect(q).tiles.size() == 24);
    // Fog: everything unseen but one visible cell and one explored cell.
    FovMemory &fov = w.resources().insert<FovMemory>();
    fov.visible.resize(6, 4, 0);
    fov.explored.resize(6, 4, 0);
    fov.visible(1, 1) = 1;
    fov.explored(1, 1) = 1;
    fov.explored(2, 1) = 1;
    level.h.ticks(1);
    CHECK(collect(q).fog.empty()); // FovMemory alone: no fog without FogStyle
    w.resources().insert<FogStyle>();
    level.h.ticks(1);
    d = collect(q);
    CHECK(d.fog.size() == 23);
    int explored = 0;
    for (const DrawQuad &quad : d.fog) {
        if (quad.v[0].color.a == 140) {
            ++explored;
            CHECK(quad.v[0].pos == Vec2{132, 66});
        } else {
            CHECK(quad.v[0].color.a == 255);
        }
        CHECK(quad_at(d.fog, {116, 66}) == nullptr);
    }
    CHECK(explored == 1);
    // update_fov from the hero's cell lights the open room.
    update_fov(fov, w.resources().get<TileWorld>(), {2, 2}, 8);
    level.h.ticks(1);
    d = collect(q);
    CHECK(d.fog.size() < 10);
}

TEST_CASE("tilemap plugin: every system is allocation-free in steady state") {
    Level level;
    const Entity map = level.add_map("maps/forest.tmj");
    RenderQueue &q = level.app().insert_resource<RenderQueue>();
    World &w = level.world();
    w.resources().insert<CameraView>(view_of(Rect{100, 50, 96, 64}));
    FovMemory &fov = w.resources().insert<FovMemory>();
    w.resources().insert<FogStyle>();
    level.h.ticks(1);
    update_fov(fov, w.resources().get<TileWorld>(), {1, 1}, 4);
    Entity hero;
    for (auto [e, nm] : w.view<Name>()) {
        if (nm.id == "start"_id) hero = e;
    }
    REQUIRE(w.alive(hero));
    w.add<GridMover>(hero, GridMover{.from = {1, 1}, .to = {1, 1}, .ticks = 4});
    w.add<PathRequest>(hero, PathRequest{.goal = {4, 2}});
    const auto play = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            level.h.ticks(1);
            q.clear(); // render_clear's job (FrameEnd +9000) in a full engine
            const IVec2 dir = (i / 8) % 2 == 0 ? IVec2{1, 0} : IVec2{-1, 0};
            (void)try_step(w, hero, dir);
            set_tile(w, map, "ground"_id, {3, 2}, (i % 2 == 0) ? 2u : 1u);
            if (i % 5 == 0) w.add<PathRequest>(hero, PathRequest{.goal = {4, 1}});
        }
    };
    play(120);
    CHECK_NO_ALLOC(play(600));
    CHECK(w.has<PathRoute>(hero));
}

// ---- determinism --------------------------------------------------------------------------------

namespace {

// Walkers step along their routes (Corners legs) whenever their GridMover is idle.
void step_walkers(Ctx &ctx) {
    World &w = ctx.world();
    for (auto [e, gp] : w.view<GridPosition>()) {
        PathRoute *route = w.try_get<PathRoute>(e);
        const GridMover *mv = w.try_get<GridMover>(e);
        if (route == nullptr || (mv != nullptr && mv->moving)) continue;
        while (route->next < route->count && route->cells[route->next] == gp.cell)
            ++route->next;
        if (route->next >= route->count) continue;
        const IVec2 to = route->cells[route->next];
        const IVec2 dir{(to.x > gp.cell.x) - (to.x < gp.cell.x),
                        (to.y > gp.cell.y) - (to.y < gp.cell.y)};
        (void)try_step(w, e, dir, StepRules{.diagonal = true});
    }
}

void add_movers(Ctx &ctx) {
    World &w = ctx.world();
    const TileWorld &tw = w.resources().get<TileWorld>();
    for (auto [e, gp] : w.view<GridPosition>()) {
        w.commands().add<GridMover>(e, GridMover{.from = gp.cell, .to = gp.cell, .ticks = 3});
        w.commands().add<Transform>(e, Transform{.position = tw.tile_center(gp.cell)});
    }
}

std::uint64_t run_walkers(int ticks, int undo_at = -1, int undo_ticks = 0) {
    test::HeadlessApp h;
    h.add<AssetsPlugin>();
    h.add<PhysicsPlugin>();
    h.add<TilemapPlugin>(TilemapConfig{.path_searches_per_tick = 2, .max_path_cells = 4096});
    h.app().add_startup(&start_paths, "start_paths");
    h.app().add_startup(&add_movers, "add_movers");
    h.app().add_system({.stage = Stage::Simulate, .fn = &repath, .name = "repath", .order = -1200});
    h.app().add_system(Stage::Simulate, &step_walkers, "step_walkers");
    World &w = h.app().main_world();
    WorldSnapshot snap;
    for (int t = 0; t < ticks; ++t) {
        if (t == undo_at) w.snapshot_into(snap);
        h.ticks(1);
    }
    int moved = 0;
    for (auto [e, gp] : w.view<GridPosition>()) {
        (void)e;
        moved += gp.cell.x > 2 ? 1 : 0;
    }
    CHECK(moved >= 3); // they walked (queueing at the one gap, blocking each other)
    if (undo_at >= 0) {
        // Undo, then replay the same ticks: the same state again.
        const std::uint64_t first = w.state_hash();
        w.restore(snap);
        h.ticks(undo_ticks);
        CHECK(w.state_hash() == first);
    }
    return w.state_hash();
}

} // namespace

TEST_CASE("tilemap plugin: path requests and grid movers are deterministic, also across an undo") {
    const std::uint64_t a = run_walkers(90);
    const std::uint64_t b = run_walkers(90);
    CHECK(a == b);
    CHECK(run_walkers(91) != a);
    (void)run_walkers(90, 60, 30);
}

// ---- MapObjectRef and anchors, set_layer, the tick sync and PathRequest::start_offset ---------

namespace {

Entity named(World &w, StrId name) {
    for (auto [e, nm] : w.view<Name>()) {
        if (nm.id == name) return e;
    }
    return {};
}

} // namespace

TEST_CASE("tilemap plugin: spawned objects carry a MapObjectRef; \"anchor\": \"bottom\" spawns at the feet") {
    Level level;
    test::write_test_file(level.dir, "maps/yard.map.json", R"({"legend": {".": "floor"},
      "rows": ["....", "...."],
      "objects": [{"type": "hero", "name": "feet", "x": 1, "y": 0, "anchor": "bottom", "hp": 3},
                  {"type": "coin", "name": "point", "x": 2, "y": 1, "w": 0, "h": 0},
                  {"type": "sign", "x": 0, "y": 0, "text": "hello"}]})");
    const Entity yard = level.add_map("maps/yard.map.json", {100, 50}, false);
    const Entity forest = level.add_map("maps/forest.tmj", {0, 0});
    level.h.frames(1);
    World &w = level.world();
    const AssetServer &assets = level.assets();
    // Bottom anchor: the bottom centre of the cell; the cell is still the one under the centre.
    const Entity hero = named(w, "feet"_id);
    REQUIRE(w.alive(hero));
    CHECK(w.get<Transform>(hero).position == Vec2{124, 66});
    CHECK(w.get<GridPosition>(hero).cell == IVec2{1, 0});
    REQUIRE(w.has<MapObjectRef>(hero));
    CHECK(w.get<MapObjectRef>(hero).map == yard);
    CHECK(w.get<MapObjectRef>(hero).object == 0);
    const MapObject *o = map_object(w, assets, w.get<MapObjectRef>(hero));
    REQUIRE(o != nullptr);
    CHECK(prop_int(o->props, "hp"_id) == 3);
    CHECK(prop_or(o->props, "anchor"_id) == "bottom");
    // A point object (w = h = 0) spawns at the point.
    const Entity coin = named(w, "point"_id);
    REQUIRE(w.alive(coin));
    CHECK(w.get<Transform>(coin).position == Vec2{132, 66});
    CHECK(w.get<MapObjectRef>(coin).object == 1);
    // Tiled objects get one too; non-prefab objects are not spawned.
    const Entity start = named(w, "start"_id);
    REQUIRE(w.alive(start));
    CHECK(w.get<MapObjectRef>(start).map == forest);
    o = map_object(w, assets, w.get<MapObjectRef>(start));
    REQUIRE(o != nullptr);
    CHECK(o->name == "start"_id);
    CHECK(w.count<MapObjectRef>() == 4); // yard: hero, coin; forest: hero, coin
    // Out of range, or a map entity that is gone: null.
    CHECK(map_object(w, assets, MapObjectRef{yard, 3}) == nullptr);
    CHECK(map_object(w, assets, MapObjectRef{Entity{}, 0}) == nullptr);
    const MapObjectRef ref = w.get<MapObjectRef>(hero);
    const MapObject *p = nullptr;
    CHECK_NO_ALLOC(p = map_object(w, assets, ref));
    CHECK(p == &assets.get(w.get<Tilemap>(yard).map)->objects[0]);
    w.destroy(yard);
    CHECK(map_object(w, assets, ref) == nullptr);
}

TEST_CASE("tilemap plugin: set_layer replaces a whole layer: one version bump, one TileWorld refill") {
    Level level;
    const Entity map = level.add_map("maps/forest.tmj");
    World &w = level.world();
    const Grid<TileId> walls(6, 4, 2); // local 1 of the ground set: solid, opaque, cost 0
    CHECK_FALSE(set_layer(w, map, "ground"_id, walls)); // no instance before the first sync
    level.h.frames(1);
    const std::uint32_t version = w.get<TilemapInstance>(map).version;
    const std::uint32_t tw_version = w.resources().get<TileWorld>().version;
    CHECK(set_layer(w, map, "ground"_id, walls));
    CHECK(w.get<TilemapInstance>(map).version == version + 1);
    const TileWorld &tw = w.resources().get<TileWorld>();
    CHECK(tw.version == tw_version + 1);
    CHECK(tw.origin == Vec2{100, 50});
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 6; ++x) {
            CHECK(tw.at({x, y}) == TileShape::Solid);
            CHECK(tw.cost(x, y) == 0);
        }
    }
    // The shared asset is untouched.
    CHECK(level.assets().get(w.get<Tilemap>(map).map)->layers[0].tiles(1, 1) == 1);
    // Wrong size or unknown layer: false, nothing changes.
    CHECK_FALSE(set_layer(w, map, "ground"_id, Grid<TileId>(5, 4, 1)));
    CHECK_FALSE(set_layer(w, map, "nope"_id, walls));
    CHECK(w.get<TilemapInstance>(map).version == version + 1);
    // A layer without collision changes the instance only.
    CHECK(set_layer(w, map, "over"_id, Grid<TileId>(6, 4, 3)));
    CHECK(w.get<TilemapInstance>(map).version == version + 2);
    CHECK(tw.version == tw_version + 1);
    // Steady state: no allocation; the edit survives later frames (the asset did not change).
    const Grid<TileId> floor(6, 4, 1);
    bool ok = false;
    CHECK_NO_ALLOC(ok = set_layer(w, map, "ground"_id, floor));
    CHECK(ok);
    CHECK(tw.at({0, 0}) == TileShape::Empty);
    CHECK(tw.cost(0, 0) == kDefaultTileCost);
    level.h.frames(2);
    CHECK(tile_at(w.get<TilemapInstance>(map), "ground"_id, {0, 0}) == 1);
    // Passing the layer's own grid is a plain refresh.
    const Grid<TileId> &own = find_layer(w.get<TilemapInstance>(map).layers, "ground"_id)->tiles;
    CHECK(set_layer(w, map, "ground"_id, own));
}

namespace {

struct TickSyncProbe {
    AssetId<TilemapAsset> map;
    bool add = false;
    Entity added;
    bool seen_instance = false;
    int seen_width = 0;
    int seen_heroes = 0;
};
TickSyncProbe g_probe;

// TickBegin -1950: a Tilemap appears at a tick boundary (as when a scene is entered at TickBegin).
void add_map_now(Ctx &ctx) {
    if (!g_probe.add || g_probe.added) return;
    World &w = ctx.world();
    g_probe.added = w.create();
    w.add<Transform>(g_probe.added);
    w.add<Tilemap>(g_probe.added, Tilemap{.map = g_probe.map});
}

// Simulate -2000: the same tick's rules already see the instance, TileWorld and objects.
void see_map(Ctx &ctx) {
    World &w = ctx.world();
    if (!g_probe.added || g_probe.seen_instance) return;
    g_probe.seen_instance = w.has<TilemapInstance>(g_probe.added);
    g_probe.seen_width = w.resources().get<TileWorld>().shapes.width();
    for (auto [e, gp] : w.view<GridPosition>()) {
        (void)e;
        (void)gp;
        ++g_probe.seen_heroes;
    }
}

} // namespace

TEST_CASE("tilemap plugin: a Tilemap added at a tick boundary is synced before that tick's rules") {
    g_probe = {};
    Level level;
    level.app().add_system(
        {.stage = Stage::TickBegin, .fn = &add_map_now, .name = "add_map_now", .order = -1950});
    level.app().add_system(
        {.stage = Stage::Simulate, .fn = &see_map, .name = "see_map", .order = -2000});
    g_probe.map = level.assets().load<TilemapAsset>("maps/forest.tmj");
    level.h.ticks(2);
    CHECK_FALSE(g_probe.added);
    g_probe.add = true;
    level.h.ticks(1);
    REQUIRE(g_probe.added);
    CHECK(g_probe.seen_instance);
    CHECK(g_probe.seen_width == 6);
    CHECK(g_probe.seen_heroes == 1);
    // Idempotent: further ticks and frames copy and spawn nothing more.
    World &w = level.world();
    const std::size_t entities = w.entity_count();
    const std::uint32_t version = w.get<TilemapInstance>(g_probe.added).version;
    level.h.ticks(3);
    CHECK(w.entity_count() == entities);
    CHECK(w.get<TilemapInstance>(g_probe.added).version == version);
    g_probe = {};
}

TEST_CASE("tilemap plugin: PathRequest::start_offset starts a feet-anchored body at its centre") {
    test::HeadlessApp h;
    h.add<AssetsPlugin>();
    h.add<PhysicsPlugin>();
    h.add<TilemapPlugin>(TilemapConfig{.max_path_cells = 4096});
    World &w = h.app().main_world();
    TileWorld &tw = w.resources().get<TileWorld>();
    tw.resize(10, 8, 16, {0, 0});
    tw.set({3, 5}, TileShape::Solid, kTileOpaque); // the feet stand at the bottom of this cell
    tw.cost(3, 5) = 0;
    const Vec2 feet = tw.tile_center({3, 5}) + Vec2{0, 6};
    const Vec2 offset{0, -16}; // the body centre is in cell (3, 4)
    const Entity body = w.create();
    w.add<Transform>(body, Transform{.position = feet});
    w.add<PathRequest>(body, PathRequest{.goal = {8, 4}, .start_offset = offset});
    const Entity by_feet = w.create();
    w.add<Transform>(by_feet, Transform{.position = feet});
    w.add<PathRequest>(by_feet, PathRequest{.goal = {8, 4}});
    h.ticks(1);
    REQUIRE(w.has<PathRoute>(body));
    PathRoute route = w.get<PathRoute>(body);
    CHECK(route.complete);
    REQUIRE(route.count > 0);
    CHECK(route.cells[route.count - 1] == IVec2{8, 4});
    const Vec2 dir = follow_route(route, tw, feet + offset);
    CHECK(dir.x > 0.9f);
    // From the feet cell (solid) there is no route.
    REQUIRE(w.has<PathRoute>(by_feet));
    CHECK(w.get<PathRoute>(by_feet).count == 0);
    CHECK_FALSE(w.get<PathRoute>(by_feet).complete);
}

// ---- Regressions --------------------------------------------------------------------------------

TEST_CASE("tilemap plugin: a world restored before its map is Ready keeps its tile edits") {
    // The run that saved: the wall at (1, 0) dug out, a wall built at (2, 1).
    std::string saved;
    std::uint32_t saved_version = 0;
    {
        Level level;
        const Entity map = level.add_map("maps/forest.tmj");
        level.h.frames(1);
        World &w = level.world();
        set_tile(w, map, "ground"_id, {1, 0}, 1);
        set_tile(w, map, "ground"_id, {2, 1}, 2);
        saved_version = w.get<TilemapInstance>(map).version;
        DataWriter out;
        lam_write(w.get<TilemapInstance>(map), out, level.app().serialize_context());
        const Result<std::string> json = out.finish();
        REQUIRE(json.ok());
        saved = *json;
    }
    // A fresh process loads the save while the map is still loading (here: not there yet, so the
    // asset is not Ready; an async load is Queued or Loading at this point).
    test::CaptureLog log; // the failed first load logs an error
    Level level;
    std::filesystem::remove(level.dir.file("maps/forest.tmj"));
    AssetServer &assets = level.assets();
    World &w = level.world();
    const AssetId<TilemapAsset> id = assets.load<TilemapAsset>("maps/forest.tmj");
    REQUIRE(assets.state(id) != LoadState::Ready);
    const Entity map = w.create();
    w.add<Transform>(map, Transform{.position = {100, 50}});
    w.add<Tilemap>(map, Tilemap{.map = id});
    TilemapInstance inst;
    Result<DataDocument> doc = DataDocument::parse(saved, "save.json");
    REQUIRE(doc.ok());
    DataReader root = doc->root();
    REQUIRE(lam_read(inst, root, level.app().serialize_context()).ok());
    REQUIRE(doc->finish().ok());
    REQUIRE(inst.map == id); // the saved path resolves to the loading asset
    w.add<TilemapInstance>(map, std::move(inst));
    WorldSnapshot snap;
    w.snapshot_into(snap);
    w.restore(snap); // what deserialize_world does; the restore hook runs
    const auto instance_hash = [&] {
        Hasher h;
        lam_hash(w.get<TilemapInstance>(map), h);
        return h.value();
    };
    const std::uint64_t hash = instance_hash();
    CHECK(w.get<TilemapInstance>(map).asset_version == 0);
    level.h.frames(1); // still not Ready: nothing happens
    CHECK(instance_hash() == hash);
    // The map arrives (an async load finishing; here the file appears and is reloaded).
    test::write_test_file(level.dir, "maps/forest.tmj", kForest);
    REQUIRE(assets.reload(id).ok());
    REQUIRE(assets.state(id) == LoadState::Ready);
    REQUIRE(assets.version(id) >= 1u);
    level.h.frames(1);
    const TilemapInstance &now = w.get<TilemapInstance>(map);
    CHECK(tile_at(now, "ground"_id, {1, 0}) == 1); // the saved edits, not the asset's walls
    CHECK(tile_at(now, "ground"_id, {2, 1}) == 2);
    CHECK(now.version == saved_version);
    CHECK(now.asset_version == assets.version(id));
    CHECK(instance_hash() == hash); // as saved
    const TileWorld &tw = w.resources().get<TileWorld>();
    CHECK(tw.shapes.width() == 6); // TileWorld is filled from the adopted layers
    CHECK(tw.at({1, 0}) == TileShape::Empty);
    CHECK(tw.at({2, 1}) == TileShape::Solid);
    level.h.frames(2);
    CHECK(tile_at(w.get<TilemapInstance>(map), "ground"_id, {2, 1}) == 2);
    // A real hot reload afterwards still wins over edits.
    assets.mark_modified(id);
    level.h.frames(1);
    CHECK(tile_at(w.get<TilemapInstance>(map), "ground"_id, {2, 1}) == 1);
    CHECK(tw.at({2, 1}) == TileShape::Empty);
}

TEST_CASE("tilemap plugin: the collision map is the first in view order, also after an undo") {
    Level level;
    test::write_test_file(level.dir, "maps/room.map.json", R"({
      "legend": {"#": "wall", ".": "floor"},
      "terrains": {"wall": {"shape": "solid", "flags": ["opaque"], "cost": 0}},
      "rows": ["#####", "#...#", "#####"]})");
    test::write_test_file(level.dir, "maps/level.txt", "####\n#..#\n#..#\n####\n");
    const Entity a = level.add_map("maps/room.map.json", {0, 0}); // 5 x 3, first in view order
    const Entity b = level.add_map("maps/level.txt", {0, 0});     // 4 x 4
    level.h.frames(1);
    World &w = level.world();
    REQUIRE(w.has<TilemapInstance>(b));
    const TileWorld &tw = w.resources().get<TileWorld>();
    REQUIRE(tw.shapes.width() == 5);
    WorldSnapshot snap;
    w.snapshot_into(snap);
    // a stops building collision: b takes over. Undo: a is first again and owns TileWorld, as at
    // the snapshot, whichever map owned it just before the undo.
    w.get<Tilemap>(a).build_collision = false;
    level.h.frames(1);
    CHECK(tw.shapes.width() == 4);
    w.restore(snap);
    CHECK(tw.shapes.width() == 5); // refilled by the restore hook
    CHECK(tw.at({2, 1}) == TileShape::Empty);
    level.h.frames(1);
    CHECK(tw.shapes.width() == 5);
    // The same when a was destroyed (a load in a fresh process is the same question).
    w.destroy(a);
    level.h.frames(1);
    CHECK(tw.shapes.width() == 4);
    w.restore(snap);
    CHECK(tw.shapes.width() == 5);
    level.h.frames(1);
    CHECK(tw.shapes.width() == 5);
    // No sticky owner: turning a off and on again hands TileWorld back to it.
    w.get<Tilemap>(a).build_collision = false;
    level.h.frames(1);
    CHECK(tw.shapes.width() == 4);
    w.get<Tilemap>(a).build_collision = true;
    level.h.frames(1);
    CHECK(tw.shapes.width() == 5);
}

TEST_CASE(
    "tilemap plugin: tiles larger than a cell show when they reach in from below or the left") {
    Level level;
    // 4 x 4 cells of 16 px, every cell a 32 px tree: a tree covers its cell and the cells above
    // and to the right of it.
    test::write_test_file(level.dir, "maps/trees.tmj", R"({
      "type": "map", "tiledversion": "1.10.2", "orientation": "orthogonal", "infinite": false,
      "width": 4, "height": 4, "tilewidth": 16, "tileheight": 16,
      "tilesets": [{"firstgid": 1, "name": "trees", "tilewidth": 32, "tileheight": 32,
                    "tilecount": 2, "columns": 2, "image": "trees.png", "imagewidth": 64,
                    "imageheight": 32}],
      "layers": [{"name": "trees", "type": "tilelayer", "width": 4, "height": 4, "opacity": 1,
                  "visible": true, "data": [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]}]})");
    test::write_test_file(level.dir, "maps/trees.png", make_png(64, 32));
    level.add_map("maps/trees.tmj", {0, 0}, false);
    RenderQueue &q = level.app().insert_resource<RenderQueue>();
    World &w = level.world();
    // The view spans x 16..48, y 0..32: cells x 1..2, y 0..1 lie under it; the trees of column 0
    // (x 0..32) and of row 2 (y 16..48) reach into it too. Column 3 and row 3 only touch it.
    w.resources().insert<CameraView>(view_of(Rect{16, 0, 32, 32}));
    level.h.ticks(1);
    const Drawn d = collect(q);
    CHECK(d.tiles.size() == 9);
    CHECK(quad_at(d.tiles, {0, -16}) != nullptr); // cell (0, 0)
    CHECK(quad_at(d.tiles, {0, 16}) != nullptr);  // cell (0, 2): left of and below the view
    CHECK(quad_at(d.tiles, {32, 16}) != nullptr); // cell (2, 2): below the view
    CHECK(quad_at(d.tiles, {48, 0}) == nullptr);  // cell (3, 1): only touches the right edge
    CHECK(quad_at(d.tiles, {16, 32}) == nullptr); // cell (1, 3): only touches the bottom edge
    for (const DrawQuad &quad : d.tiles)
        CHECK(quad.v[2].pos - quad.v[0].pos == Vec2{32, 32});
}

TEST_CASE("tilemap plugin: a tileset whose tiles do not fit in its image fails to load") {
    test::CaptureLog log;
    Level level;
    AssetServer &assets = level.assets();
    // tiles.png is 64 x 32: 4 x 2 tiles of 16 px.
    test::write_test_file(level.dir, "maps/fits.tileset.json",
                          R"({"image": "tiles.png", "tile_size": 16, "count": 8})");
    test::write_test_file(level.dir, "maps/narrow.tileset.json",
                          R"({"image": "tiles.png", "tile_size": 16, "columns": 2, "count": 4})");
    test::write_test_file(level.dir, "maps/count.tileset.json",
                          R"({"image": "tiles.png", "tile_size": 16, "count": 64})");
    test::write_test_file(level.dir, "maps/columns.tileset.json",
                          R"({"image": "tiles.png", "tile_size": 16, "columns": 8, "count": 8})");
    test::write_test_file(
        level.dir, "maps/huge.tileset.json",
        R"({"image": "tiles.png", "tile_size": 4096, "columns": 1048576, "count": 600000})");
    const AssetId<Tileset> fits = assets.load<Tileset>("maps/fits.tileset.json");
    REQUIRE(assets.state(fits) == LoadState::Ready);
    REQUIRE(assets.get(fits)->regions.size() == 8);
    CHECK(assets.get(fits)->region(7).source == Rect{48, 16, 16, 16});
    const AssetId<Tileset> narrow = assets.load<Tileset>("maps/narrow.tileset.json");
    REQUIRE(assets.state(narrow) == LoadState::Ready);
    CHECK(assets.get(narrow)->region(3).source == Rect{16, 16, 16, 16});
    for (const char *path :
         {"maps/count.tileset.json", "maps/columns.tileset.json", "maps/huge.tileset.json"}) {
        CAPTURE(path);
        const AssetId<Tileset> id = assets.load<Tileset>(path);
        CHECK(assets.state(id) == LoadState::Failed);
        const Error *e = assets.error(id);
        REQUIRE(e != nullptr);
        CHECK(e->message.find(path) != std::string::npos);
        CHECK(e->message.find("tiles.png") != std::string::npos);
        CHECK(e->message.find("the image is 64 x 32") != std::string::npos);
    }
}
