// Apple Trail rules tests (apple_trail_sim only: no window, no raylib).
#include <cstdlib>
#include <lam/core/alloc.h>
#include <string>
#include <lam/assets/asset_server.h>
#include <lam/core/fs.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/input_plugin.h>
#include <lam/physics/physics_plugin.h>
#include <lam/scene/save.h>
#include <lam/test/support.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/tilemap/tilemap_plugin.h>
#include "trail_bot.h"
#include "trail_rules.h"

using namespace trail;

namespace {

std::string asset_text(const char *name) {
    const char *dir = std::getenv("LAM_TEST_SOURCE_DIR");      // .../apple_trail/tests
    const std::string root = std::string(dir != nullptr ? dir : ".") + "/../assets/levels/";
    lam::Result<std::string> text = lam::read_file(root + name);
    REQUIRE_MESSAGE(text.ok(), "missing ", root + name);
    return *text;
}

Orchard orchard_from_text() {
    lam::Result<Orchard> o = parse_orchard(asset_text("orchard.txt"), "orchard.txt");
    REQUIRE_MESSAGE(o.ok(), (o.ok() ? "" : o.error().message));
    return *o;
}

int count_segments(const lam::World &w) { return static_cast<int>(w.count<Segment>()); }

} // namespace

TEST_CASE("turn buffer keeps two presses and ignores reversals and repeats") {
    TrailState s;                                               // heading Right
    queue_turn(s, Dir::Left);                                   // reversal: ignored
    queue_turn(s, Dir::Right);                                  // same heading: ignored
    CHECK(s.queued_count == 0);
    queue_turn(s, Dir::Up);
    queue_turn(s, Dir::Up);                                     // repeat of the last queued: ignored
    queue_turn(s, Dir::Left);
    queue_turn(s, Dir::Down);                                   // buffer full
    REQUIRE(s.queued_count == 2);
    CHECK(s.queued[0] == Dir::Up);
    CHECK(s.queued[1] == Dir::Left);
    s.alive = false;
    s.queued_count = 0;
    queue_turn(s, Dir::Up);                                     // dead: ignored
    CHECK(s.queued_count == 0);
}

TEST_CASE("pace quickens every four apples down to the minimum") {
    CHECK(step_ticks_for(0) == kRules.start_step_ticks);
    CHECK(step_ticks_for(3) == kRules.start_step_ticks);
    CHECK(step_ticks_for(4) == kRules.start_step_ticks - 1);
    CHECK(step_ticks_for(400) == kRules.min_step_ticks);
}

TEST_CASE("the text board parses as raw ASCII rows: walls and start") {
    const Orchard o = orchard_from_text();
    CHECK(o.width() == 28);
    CHECK(o.height() == 15);
    CHECK(o.start == lam::IVec2{6, 8});                         // the 'P' cell
    CHECK(o.wall({0, 0}));
    CHECK_FALSE(o.wall({1, 1}));
    CHECK(o.wall({11, 6}));                                     // the centre block
    CHECK(o.wall({-1, 3}));                                     // outside is solid
    CHECK(o.tiles.opaque({0, 0}));
    CHECK(o.tiles.cost(0, 0) == 0);                             // impassable for path finding
    CHECK(o.tiles.cost(1, 1) == 10);
}

TEST_CASE("the native .map.json gives the same board") {
    lam::Result<Orchard> native = parse_orchard(asset_text("orchard.map.json"), "orchard.map.json");
    REQUIRE_MESSAGE(native.ok(), (native.ok() ? "" : native.error().message));
    const Orchard text = orchard_from_text();
    CHECK(native->start == text.start);                         // legend 'P' -> a "start" object
    CHECK(native->tiles.tile_size == 16);
    CHECK(native->tiles.shapes == text.tiles.shapes);
    CHECK(native->tiles.flags == text.tiles.flags);
    CHECK(native->tiles.cost == text.tiles.cost);
}

TEST_CASE("a map that breaks the native schema is an error, not a silent fallback") {
    // An older layout (integer legend values, rows inside a layer) must fail loudly.
    const char *old = R"({"width": 3, "height": 1, "legend": {".": 0, "#": 1},
                          "layers": [{"name": "walls", "rows": ["#.#"]}]})";
    CHECK_FALSE(parse_orchard(old, "old.map.json").ok());
    const char *walled_start = R"({"terrains": {"wall": {"shape": "solid"}},
                                   "legend": {"#": "wall", "P": {"terrain": "wall", "object": "start"}},
                                   "rows": ["#P#"]})";
    CHECK_FALSE(parse_orchard(walled_start, "walled.map.json").ok());
}

TEST_CASE("bot: BFS heads for the apple, falls back to the roomiest turn, never reverses") {
    lam::Grid<std::uint8_t> blocked(7, 5, 0);
    BfsScratch scratch;
    const lam::IVec2 apple[] = {{5, 2}};
    CHECK(autoplay_choice(blocked, {1, 2}, Dir::Right, apple, scratch) == Dir::Right);
    const lam::IVec2 above[] = {{1, 0}};
    CHECK(autoplay_choice(blocked, {1, 2}, Dir::Right, above, scratch) == Dir::Up);
    // Apple behind the traveller: the reversal is never chosen, a turn is.
    const lam::IVec2 behind[] = {{0, 2}};
    const std::optional<Dir> d = autoplay_choice(blocked, {1, 2}, Dir::Right, behind, scratch);
    REQUIRE(d.has_value());
    CHECK(*d != Dir::Left);
    // Walled in on three sides with no target: the only open turn.
    blocked.set(2, 2, 1);
    blocked.set(1, 3, 1);
    CHECK(autoplay_choice(blocked, {1, 2}, Dir::Right, {}, scratch) == Dir::Up);
    blocked.set(1, 1, 1);
    CHECK_FALSE(autoplay_choice(blocked, {1, 2}, Dir::Right, {}, scratch).has_value());
}

TEST_CASE("start_run spawns the traveller, a linked trail of three and one apple") {
    lam::World w;
    start_run(w, orchard_from_text());
    const TrailState &s = w.resources().get<TrailState>();
    CHECK(s.alive);
    CHECK(s.length == kRules.start_length);
    CHECK(count_segments(w) == 1 + kRules.start_length);
    CHECK(w.get<lam::GridPosition>(s.head).cell == lam::IVec2{6, 8});
    // Walk head -> tail through Segment::behind.
    int walked = 0;
    lam::IVec2 expect{6, 8};
    for (lam::Entity e = s.head; e; e = w.get<Segment>(e).behind, ++walked) {
        CHECK(w.get<lam::GridPosition>(e).cell == expect);
        expect.x -= 1;
    }
    CHECK(walked == 1 + kRules.start_length);
    int apples = 0;
    for (auto [e, apple, at] : w.view<Apple, lam::GridPosition>()) {
        ++apples;
        CHECK(apple.kind == AppleKind::Red);
        CHECK(w.resources().get<lam::TileWorld>().at(at.cell) == lam::TileShape::Empty);
    }
    CHECK(apples == 1);
}

TEST_CASE("same seed, same board: start_run is deterministic") {
    lam::World a(lam::WorldConfig{});
    lam::World b(lam::WorldConfig{});
    a.reseed(42);
    b.reseed(42);
    start_run(a, orchard_from_text());
    start_run(b, orchard_from_text());
    CHECK(a.state_hash() == b.state_hash());
}

TEST_CASE("undo round trip: a History snapshot restores the run, TrailState included") {
    lam::World w;
    start_run(w, orchard_from_text());
    const std::uint64_t before = w.state_hash();
    lam::History history;
    history.push(w);

    TrailState &s = w.resources().get<TrailState>();
    queue_turn(s, Dir::Up);
    s.score = 990;
    s.eaten = 99;
    CHECK(w.state_hash() != before);                            // the resource alone is hashed
    w.destroy(s.tail);
    CHECK(w.state_hash() != before);

    REQUIRE(history.undo(w));
    CHECK(w.state_hash() == before);
    const TrailState &back = w.resources().get<TrailState>();
    CHECK(back.score == 0);
    CHECK(back.queued_count == 0);
    CHECK(count_segments(w) == 1 + kRules.start_length);
}

namespace {
// Without lam_input, a test system plays the bot's part: the same choice the InputDriver makes,
// queued straight into the turn buffer at Stage::Input (where trail.steer would).
TrailBot *g_bot = nullptr;
void manual_steer(lam::Ctx &ctx) {
    if (const std::optional<Dir> d = g_bot->choose(ctx.world())) queue_turn(ctx.local<TrailState>(), *d);
}
// The engine plugins the rules need (TilemapPlugin requires AssetsPlugin and PhysicsPlugin).
void add_engine(lam::test::HeadlessApp &h) {
    h.add<lam::AssetsPlugin>();
    h.add<lam::PhysicsPlugin>();
    h.add<lam::TilemapPlugin>();
}
// A headless App with the rules on the main world, steered by manual_steer.
void setup_rules_app(lam::test::HeadlessApp &h) {
    add_engine(h);
    h.add<TrailRulesPlugin>();
    h.app().add_system(lam::Stage::Input, manual_steer, "test.manual_steer");
    start_run(h.app().main_world(), orchard_from_text());
}
} // namespace

TEST_CASE("rules: a bot-steered run eats, scores, speeds up and grows the trail") {
    lam::test::HeadlessApp h(lam::AppConfig{.seed = 3});
    TrailBot bot(Controls{});
    g_bot = &bot;
    setup_rules_app(h);
    lam::World &w = h.app().main_world();
    for (int i = 0; i < 60 * 90 && w.resources().get<TrailState>().eaten < 12; ++i) h.ticks(1);
    const TrailState &s = w.resources().get<TrailState>();
    INFO("steps=", s.steps, " tick=", w.tick(), " app_tick=", h.app().tick(), " head=",
         w.get<lam::GridPosition>(s.head).cell.x, ",", w.get<lam::GridPosition>(s.head).cell.y);
    CHECK(s.alive);
    CHECK(s.eaten >= 12);
    CHECK(s.score >= s.eaten * kRules.red_points);
    CHECK(s.step_ticks == step_ticks_for(s.eaten));
    CHECK(s.length + s.grow >= kRules.start_length + s.eaten);   // every apple adds at least one piece
    CHECK(count_segments(w) == 1 + s.length);
    CHECK(s.steps > 0);
    g_bot = nullptr;
}

TEST_CASE("rules: same seed and same steering give the same state hash") {
    const auto run = [](std::uint64_t seed) {
        lam::test::HeadlessApp h(lam::AppConfig{.seed = seed});
        TrailBot bot(Controls{});
        g_bot = &bot;
        setup_rules_app(h);
        h.ticks(900);
        g_bot = nullptr;
        return h.app().main_world().state_hash();
    };
    const std::uint64_t a = run(11);
    CHECK(a == run(11));
    CHECK(a != run(12));                                        // apples land elsewhere
}

TEST_CASE("save round trip: serialize_world -> deserialize_world continues bit-identically") {
    lam::test::HeadlessApp h(lam::AppConfig{.seed = 5});
    TrailBot bot(Controls{});
    g_bot = &bot;
    setup_rules_app(h);
    lam::World &w = h.app().main_world();
    h.ticks(300);
    lam::DataWriter out;
    REQUIRE(lam::serialize_world(w, out, h.app().serialize_context()).ok());   // one JSON object
    lam::Result<std::string> saved = out.finish();
    REQUIRE(saved.ok());
    h.ticks(300);
    const std::uint64_t continuous = w.state_hash();

    lam::Result<lam::DataDocument> doc = lam::DataDocument::parse(*saved, "save.json");
    REQUIRE(doc.ok());
    lam::DataReader root = doc->root();
    REQUIRE(lam::deserialize_world(w, root, h.app().serialize_context()).ok());
    h.ticks(300);
    CHECK(w.state_hash() == continuous);
    g_bot = nullptr;
}

TEST_CASE("bot run: the InputDriver eats ten apples on the orchard") {
    lam::test::HeadlessApp h;
    add_engine(h);
    h.add<lam::InputPlugin>();
    h.add<TrailRulesPlugin>();
    lam::App &app = h.app();
    lam::ActionMap &map = app.res<lam::ActionMap>();
    const Controls c{.up = map.button("up"), .down = map.button("down"), .left = map.button("left"),
                     .right = map.button("right"), .confirm = map.button("confirm"), .pause = map.button("pause")};
    app.insert_resource<Controls>(c);
    TrailBot bot(c);
    app.res<lam::InputSystem>().set_driver(&bot);
    start_run(app.main_world(), orchard_from_text());

    for (int i = 0; i < 60 * 120 && app.main_world().resources().get<TrailState>().eaten < 10; ++i) h.ticks(1);
    const TrailState &s = app.main_world().resources().get<TrailState>();
    CHECK(s.eaten >= 10);
    CHECK(s.score >= 10 * kRules.red_points);
    CHECK(s.length >= kRules.start_length + 10 - 1);            // growth lags one step behind eating
    app.res<lam::InputSystem>().set_driver(nullptr);
}

TEST_CASE("replayed run: the same seed and bot give the same hash") {
    const auto run = [] {
        lam::test::HeadlessApp h(lam::AppConfig{.seed = 7});
        add_engine(h);
        h.add<lam::InputPlugin>();
        h.add<TrailRulesPlugin>();
        lam::App &app = h.app();
        lam::ActionMap &map = app.res<lam::ActionMap>();
        const Controls c{.up = map.button("up"), .down = map.button("down"), .left = map.button("left"),
                         .right = map.button("right"), .confirm = map.button("confirm"), .pause = map.button("pause")};
        app.insert_resource<Controls>(c);
        TrailBot bot(c);
        app.res<lam::InputSystem>().set_driver(&bot);
        start_run(app.main_world(), orchard_from_text());
        h.ticks(1500);
        const std::uint64_t hash = app.main_world().state_hash();
        app.res<lam::InputSystem>().set_driver(nullptr);
        return hash;
    };
    CHECK(run() == run());
}

TEST_CASE("steady state: the bot's search allocates nothing after its first call") {
    // The rules allocate only when the entity count changes (a spawned piece or apple grows the
    // pools and the next GridIndex rebuild); the bot runs every tick and must never allocate.
    lam::test::HeadlessApp h(lam::AppConfig{.seed = 7});
    TrailBot bot(Controls{});
    g_bot = &bot;
    setup_rules_app(h);
    lam::World &w = h.app().main_world();
    (void)bot.choose(w);                                        // warm-up: sizes the scratch once
    std::uint64_t allocations = 0;
    for (int i = 0; i < 1200; ++i) {
        const lam::AllocationScope scope;
        (void)bot.choose(w);
        allocations += scope.count();
        h.ticks(1);
    }
    if (lam::allocation_counting_enabled()) CHECK(allocations == 0u);
    CHECK(w.resources().get<TrailState>().eaten > 0);           // it ate, so targets and the trail changed
    g_bot = nullptr;
}
