// Shooter rules, headless: layout/wave/reflection checks, then gameplay on the real modules (bot
// milestones, stress, same-seed hashes, undo and save round trips, the restart intent).
#include <array>
#include <cstdlib>
#include <string>
#include <lam/core/alloc.h>
#include <lam/core/data.h>
#include <lam/core/default_plugins.h>
#include <lam/ecs/component.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/driver.h>
#include <lam/physics/tile_world.h>
#include <lam/scene/save.h>
#include <lam/test/support.h>
#include <shooter/arena.h>
#include <shooter/bot.h>
#include <shooter/components.h>
#include <shooter/sim_plugin.h>

using namespace lam;
using namespace shooter;

namespace {
std::string asset_root() {
    const char *src = std::getenv("LAM_TEST_SOURCE_DIR");
    return src ? std::string(src) + "/../assets" : std::string("assets");
}

// The headless game minus presentation: same plugins and rules as `shooter --headless --bot`.
struct Game {
    ShooterBot bot;                                             // first: outlives the App
    test::HeadlessApp h;
    Entity player;
    explicit Game(ArenaConfig config = {}, bool with_bot = true, std::uint64_t seed = 7)
        : h(AppConfig{.seed = seed}) {
        App &app = h.app();
        app.add_plugins(
            DefaultPlugins{.headless = true, .scenes = false, .assets = {.root = asset_root()}});
        app.add_plugin<ShooterSimPlugin>();
        const Status s = app.res<PrefabLibrary>().load("prefabs.json", app.serialize_context());
        REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
        player = setup_arena(app, app.main_world(), config);
        if (with_bot) app.res<InputSystem>().set_driver(&bot);
    }
    App &app() { return h.app(); }
    World &world() { return h.app().main_world(); }
    Arena &arena() { return world().resources().get<Arena>(); }
};
}

TEST_CASE("arena layout: walled, rectangular, with spawn pads and a floor start") {
    int pads = 0, starts = 0;
    for (int y = 0; y < kArenaH; ++y) {
        const std::string_view row = kArenaRows[static_cast<std::size_t>(y)];
        REQUIRE(row.size() == static_cast<std::size_t>(kArenaW));
        CHECK(row.front() == '#');
        CHECK(row.back() == '#');
        for (char c : row) {
            pads += c == 'S';
            starts += c == 'P';
            if (y == 0 || y == kArenaH - 1) CHECK(c == '#');
        }
    }
    CHECK(pads >= 4);
    CHECK(starts == 1);
}

TEST_CASE("waves grow into the hundreds; --enemies overrides the size") {
    CHECK(wave_size(1, 0) == 16);
    CHECK(wave_size(2, 0) > wave_size(1, 0));
    CHECK(wave_size(10, 0) >= 300);
    CHECK(wave_size(3, 2000) == 2000);
}

TEST_CASE("build_arena_tiles fills TileWorld shapes and flow-field costs from the layout") {
    World w;
    w.resources().insert<TileWorld>();
    build_arena_tiles(w);
    const TileWorld &t = w.resources().get<TileWorld>();
    CHECK(t.shapes.width() == kArenaW);
    CHECK(t.shapes.height() == kArenaH);
    CHECK(t.at({0, 0}) == TileShape::Solid);
    CHECK(t.cost(0, 0) == 0);
    const IVec2 start = t.tile_at(player_start());
    CHECK(t.at(start) == TileShape::Empty);
    CHECK(t.cost(start.x, start.y) > 0);
    CHECK(t.at({-1, 5}) == TileShape::Solid);                  // outside_solid
}

TEST_CASE("game state is reflected for prefabs, saves and the state hash") {
    const ComponentRegistry &reg = ComponentRegistry::global();
    for (const char *name : {"Health", "Enemy", "Weapon", "Bullet", "Pickup", "Arena", "Player"}) {
        const ComponentInfo *info = reg.find(name);
        REQUIRE_MESSAGE(info != nullptr, name);
        CHECK((info->flags & kHash) != 0);
    }
}

TEST_CASE("every enemy of a wave reaches a player who stands still") {
    // Regression: spawn pads next to walls started bodies inside wall tiles (move_box then freezes
    // them on both axes), and flow directions taken at the feet snagged bodies on pillar corners.
    Game g(ArenaConfig{.god = true}, false);
    g.h.ticks(90 + 1500);                                       // intermission + 25 s of wave 1
    const Vec2 p = g.world().get<Transform>(g.player).position;
    std::size_t far = 0;
    for (auto [e, t, en] : g.world().view<Transform, Enemy>())
        if (distance_sq(t.position, p) > 64.0f * 64.0f) ++far;
    CHECK(g.world().count<Enemy>() == wave_size(1, 0));
    CHECK(far == 0);
}

TEST_CASE("the bot survives, clears waves and kills") {
    Game g;
    g.h.ticks(60 * 90);                                         // 90 s of game time
    INFO("wave " << g.arena().wave << " kills " << g.arena().total_kills << " deaths "
                 << g.arena().deaths);
    CHECK(g.arena().wave >= 3);
    CHECK(g.arena().total_kills >= 40);
}

TEST_CASE("stress mode keeps hundreds of enemies alive and ticks without allocating") {
    Game g(ArenaConfig{.stress_enemies = 600, .god = true});
    g.h.ticks(60 * 8);
    CHECK(g.world().count<Enemy>() >= 300);
    CHECK(g.arena().deaths == 0);
    // Pools, event channels and command arenas grew to their high-water marks while the wave
    // spawned (counted allocations); from here a tick must not allocate. Sanitizer builds
    // have no counting (LAM_SANITIZE): the count is skipped there, as the engine's tests do.
    if (!allocation_counting_enabled()) return;
    AllocationScope scope;
    g.h.ticks(120);
    CHECK(scope.count() == 0);
}

TEST_CASE("same seed, same run: state hashes match") {
    Game a, b;
    a.h.ticks(1200);
    b.h.ticks(1200);
    CHECK(a.world().state_hash() == b.world().state_hash());
    Game c(ArenaConfig{}, true, 8);
    c.h.ticks(1200);
    CHECK(c.world().state_hash() != a.world().state_hash());
}

TEST_CASE("undo round trip: a restored snapshot replays bit-identically") {
    Game g;
    g.h.ticks(600);
    WorldSnapshot snap;
    g.world().snapshot_into(snap);
    g.h.ticks(300);
    const std::uint64_t first = g.world().state_hash();
    g.world().restore(snap);
    g.h.ticks(300);
    CHECK(g.world().state_hash() == first);
}

TEST_CASE("save round trip: a loaded save continues bit-identically") {
    Game g;
    g.h.ticks(600);
    DataWriter out;
    REQUIRE(serialize_world(g.world(), out, g.app().serialize_context()).ok());
    Result<std::string> text = out.finish();
    REQUIRE(text.ok());
    g.h.ticks(300);
    const std::uint64_t first = g.world().state_hash();
    Result<DataDocument> doc = DataDocument::parse(*text, "save.json");
    REQUIRE(doc.ok());
    DataReader root = doc->root();
    REQUIRE(deserialize_world(g.world(), root, g.app().serialize_context()).ok());
    g.h.ticks(300);
    CHECK(g.world().state_hash() == first);
}

TEST_CASE("restart intent resets the run and keeps the session") {
    Game g;                                                     // the bot plays until it dies
    for (int i = 0; i < 60 * 600 && g.arena().phase != Phase::GameOver; ++i) g.h.ticks(1);
    REQUIRE(g.arena().phase == Phase::GameOver);
    const std::int64_t score = g.arena().score;
    g.app().res<InputSystem>().queue_intent(Intent{kIntentRestart});
    g.h.ticks(2);
    CHECK(g.arena().phase == Phase::Intermission);
    CHECK(g.arena().score == 0);
    CHECK(g.arena().wave == 0);
    CHECK(g.arena().runs == 2);
    CHECK(g.arena().deaths == 1);
    CHECK(g.arena().best == score);
    CHECK(g.world().count<Enemy>() == 0);
    const Health &h = g.world().get<Health>(find_player(g.world()));
    CHECK(h.hp == h.max_hp);
}


// ---- Engine edge cases. The first is a fixed bug (plain test); the second is a known limitation
// and stays should_fail: when the engine changes, doctest reports it.

TEST_CASE("engine: a top-down box that starts inside a solid tile can move away") {
    TileWorld tw;
    tw.resize(8, 8, 16, Vec2{0, 0});
    tw.set({2, 2}, TileShape::Solid, 0);                        // x 32..48, y 32..48
    const Rect box{36, 45, 10, 10};                             // top 3 px inside the tile
    const BoxMove r = move_box(tw, box, Vec2{0, 2});            // straight away from it
    INFO("box y " << r.box.y << " hit_x " << r.hit_x << " hit_y " << r.hit_y);
    CHECK(r.box.y > box.y);
    CHECK_FALSE(r.hit_y);
    const BoxMove deeper = move_box(tw, box, Vec2{0, -2});      // never further in
    CHECK(deeper.box.y == box.y);
    CHECK(deeper.hit_y);
}

TEST_CASE("engine: corner assist with a tiny perpendicular input" * doctest::should_fail()) {
    TileWorld tw;
    tw.resize(8, 8, 16, Vec2{0, 0});
    tw.set({2, 2}, TileShape::Solid, 0);
    const Rect box{48, 45, 10, 10};                             // flush right, 3 px below its top row
    const BoxMove exact = move_box(tw, box, Vec2{-0.5f, 0}, MoveBoxOptions{.corner_assist = 4});
    REQUIRE_FALSE(exact.hit_x);                                 // pure axis input: the assist slides
    const BoxMove analog =
        move_box(tw, box, Vec2{-0.5f, 0.001f}, MoveBoxOptions{.corner_assist = 4});
    INFO("analog box " << analog.box.x << "," << analog.box.y << " hit_x " << analog.hit_x);
    CHECK_FALSE(analog.hit_x);
}
