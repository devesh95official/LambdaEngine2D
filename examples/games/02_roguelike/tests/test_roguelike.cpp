// Roguelike rule tests. The first four need only core and ecs; the rest drive the full game
// headless (tilemap, assets, animation, scene, input) on the App's main world, played by the bot.
#include <cstdlib>
#include <doctest/doctest.h>
#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/core/default_plugins.h>
#include <lam/core/fs.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/turns.h>
#include <lam/ecs/world.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/render/sprite.h>
#include <lam/test/support.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/ui/ui.h>
#include <sim/bot.h>
#include <sim/components.h>
#include <sim/controls.h>
#include <sim/data.h>
#include <sim/run.h>
#include <sim/save.h>
#include <string>

namespace {
using namespace lam::literals;

std::string source_dir() {
    // LAM_TEST_SOURCE_DIR is set by lam_add_test: examples/games/02_roguelike
    const char *dir = std::getenv("LAM_TEST_SOURCE_DIR");
    return dir ? std::string(dir) : std::string(".");
}

// The whole game minus presentation, on the App's main world, played by the bot.
struct Game {
    rl::BotDriver bot; // declared first: outlives the App
    lam::test::HeadlessApp headless;
    explicit Game(std::uint64_t seed, bool use_bot = true)
        : headless(lam::AppConfig{.seed = seed}) {
        lam::App &app = headless.app();
        lam::DefaultPlugins plugins;
        plugins.assets.root = source_dir() + "/assets";
        app.add_plugins(plugins);
        app.add_plugin<rl::RulesPlugin>();
        const lam::Status loaded = rl::load_game_data(app);
        REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
        if (lam::Audio *audio = app.try_res<lam::Audio>()) // as the executable does (silent here)
            audio->add_bank(
                app.res<lam::AssetServer>().load<lam::SoundBank>("sfx/roguelike.sounds.json"));
        rl::setup_run(app, app.main_world(), seed);
        if (use_bot) app.res<lam::InputSystem>().set_driver(&bot);
    }
    lam::World &world() { return headless.app().main_world(); }
    const rl::RunState &run() { return *rl::run_state(world()); }
};
} // namespace

TEST_CASE("item and monster tables parse strictly") {
    const auto items = rl::parse_items(
        R"({"items": [{"id": "potion_heal", "name": "potion of healing", "effect": "heal", "power": 10}]})",
        "t.json");
    REQUIRE(items.ok());
    REQUIRE(items->size() == 1);
    CHECK(items->at(0).effect == rl::ItemEffect::Heal);
    CHECK(items->at(0).power == 10);
    CHECK(items->index_of("potion_heal"_id) == 0);
    // Unknown keys and unknown effects are data errors, never silently ignored.
    CHECK_FALSE(
        rl::parse_items(
            R"({"items": [{"id": "x", "name": "x", "effect": "heal", "power": 1, "colour": 3}]})",
            "t.json")
            .ok());
    CHECK_FALSE(
        rl::parse_items(R"({"items": [{"id": "x", "name": "x", "effect": "fly", "power": 1}]})",
                        "t.json")
            .ok());
    const auto monsters = rl::parse_monsters(
        R"({"monsters": [{"prefab": "rat", "weight": 3, "max_depth": 4}]})", "m.json");
    REQUIRE(monsters.ok());
    CHECK(monsters->at(0).max_depth == 4);
}

TEST_CASE("shipped data files parse") {
    const auto items_text = lam::read_file(source_dir() + "/assets/data/items.json");
    const auto monsters_text = lam::read_file(source_dir() + "/assets/data/monsters.json");
    REQUIRE(items_text.ok());
    REQUIRE(monsters_text.ok());
    const auto items = rl::parse_items(*items_text, "data/items.json");
    const auto monsters = rl::parse_monsters(*monsters_text, "data/monsters.json");
    REQUIRE(items.ok());
    REQUIRE(monsters.ok());
    CHECK(items->size() == 4);
    CHECK(monsters->size() == 4);
}

TEST_CASE("roguelike state snapshots, undoes and hashes through reflection") {
    lam::World w;
    rl::RunState &run = w.resources().insert<rl::RunState>();
    w.snapshot_resource<rl::RunState>(); // resources join snapshots explicitly
    const lam::Entity hero = w.create();
    w.add<rl::Stats>(hero, rl::Stats{.hp = 20, .max_hp = 20});
    w.add<rl::Inventory>(hero);
    w.add<rl::Player>(hero);
    const lam::Entity rat = w.create();
    w.add<rl::Monster>(rat, rl::Monster{.name = "rat"});
    w.add<rl::Stats>(rat);
    const std::uint64_t before = w.state_hash();

    lam::History history;
    history.push(w);
    w.get<rl::Stats>(hero).hp = 3;
    w.get<rl::Inventory>(hero).slots[0] = "potion_heal"_id;
    w.get<rl::Inventory>(hero).count = 1;
    run.depth = 4;
    w.destroy(rat);
    CHECK(w.state_hash() != before); // stats, pack, run and entities are hashed

    REQUIRE(history.undo(w));
    CHECK(w.state_hash() == before);
    CHECK(w.get<rl::Stats>(hero).hp == 20);
    CHECK(w.get<rl::Inventory>(hero).count == 0);
    CHECK(w.alive(rat));
    CHECK(w.get<rl::Monster>(rat).name.view() == "rat");
    CHECK(w.resources().get<rl::RunState>().depth == 1);
}

TEST_CASE("permadeath only erases a save of the same run") {
    lam::SaveEnvelope env{.game = "roguelike",
                          .version = rl::kSaveVersion,
                          .summary_json =
                              R"({"depth": 3, "score": 120, "run_seed": 18446744073709551557})",
                          .data_json = "{}"};
    const lam::Result<std::uint64_t> seed = rl::saved_run_seed(env);
    REQUIRE(seed.ok());
    CHECK(*seed == 18446744073709551557ull); // full 64-bit precision, not a double
    env.summary_json = R"({"depth": 3})";    // older or foreign summary: no seed, never erased
    CHECK_FALSE(rl::saved_run_seed(env).ok());
}

// ---- Full game (needs tilemap, assets, animation, scene, input) --------------------------------

TEST_CASE("bot reaches depth 2") {
    Game game(7);
    for (int i = 0; i < 80 && game.run().depth < 2 && game.run().phase != rl::Phase::Dead; ++i)
        game.headless.ticks(100);
    INFO("depth " << game.run().depth << " kills " << game.run().kills << " bot inputs "
                  << game.bot.decisions());
    CHECK(game.bot.decisions() > 0);
    CHECK((game.run().depth >= 2 || game.run().kills >= 3));
}

TEST_CASE("a floor depends only on the run seed") {
    Game a(11), b(11), c(12);
    a.headless.ticks(3); // floor 1 is carved on the first tick
    b.headless.ticks(3);
    c.headless.ticks(3);
    REQUIRE(a.run().phase == rl::Phase::Playing);
    CHECK(a.world().state_hash() == b.world().state_hash());
    CHECK(a.run().stairs == b.run().stairs);
    CHECK(a.world().state_hash() != c.world().state_hash());
}

TEST_CASE("save and load continue bit-identically") {
    Game game(5);
    game.headless.ticks(400);
    const lam::SerializeContext ctx = game.headless.app().serialize_context();
    const lam::Result<std::string> saved = rl::save_run(game.world(), ctx);
    REQUIRE(saved.ok());
    game.headless.ticks(300);
    const std::uint64_t continuous = game.world().state_hash();
    const std::uint64_t tick = game.world().tick();
    const lam::Entity hero = game.run().player;
    const lam::TextureRegion hero_region = game.world().get<lam::Sprite>(hero).region;

    const lam::Status loaded = rl::load_run(game.world(), *saved, ctx);
    REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
    CHECK(game.world().tick() + 300 == tick);
    game.headless.ticks(300);
    CHECK(game.world().state_hash() ==
          continuous); // FovMemory, TurnScheduler, TilemapInstance included
    // Sprites are not hashed: their atlas regions must come back too (the Animator rewrites them).
    const lam::TextureRegion region = game.world().get<lam::Sprite>(hero).region;
    INFO("texture before " << hero_region.texture.index << " after " << region.texture.index);
    CHECK(region.texture == hero_region.texture);
    CHECK(region.source == hero_region.source);
    // A damaged save changes nothing.
    const std::uint64_t now = game.world().state_hash();
    CHECK_FALSE(rl::load_run(game.world(), "{\"game\": \"roguelike\"}", ctx).ok());
    CHECK(game.world().state_hash() == now);
}

namespace {
// What game/scenes.cpp load_intent does on F9, minus the disk: an Input-stage system restores the
// world from a save when the kLoadGame intent arrives, in the middle of a tick.
struct PendingSave { // App resource
    std::string text;
};
void load_on_intent(lam::Ctx &ctx) {
    for (const lam::Intent &intent : lam::input_frame(ctx).intent_list())
        if (intent.kind == rl::kLoadGame) {
            const lam::Status s = rl::load_run(ctx.world(), ctx.res<PendingSave>().text,
                                               ctx.app().serialize_context());
            REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
        }
}

// Plays seed 5 for 400 ticks, saves, plays 100 more, then loads through the intent and plays 200.
std::uint64_t play_with_tick_load(std::uint32_t *depth_after_load) {
    Game game(5);
    lam::App &app = game.headless.app();
    app.insert_resource<PendingSave>();
    app.add_system({.stage = lam::Stage::Input,
                    .fn = load_on_intent,
                    .name = "test.load_on_intent",
                    .order = 0,
                    .run_if = lam::has_local<rl::RunState>});
    game.headless.ticks(400);
    const lam::Result<std::string> saved = rl::save_run(game.world(), app.serialize_context());
    REQUIRE(saved.ok());
    app.res<PendingSave>().text = *saved;
    const std::uint32_t saved_depth = game.run().depth;
    const std::uint64_t saved_tick = game.world().tick();
    game.headless.ticks(100);
    app.res<lam::InputSystem>().queue_intent(lam::Intent{.kind = rl::kLoadGame});
    game.headless.ticks(1);
    CHECK(game.run().depth == saved_depth);
    CHECK(game.world().tick() <= saved_tick + 1); // back at the save (plus at most this tick)
    *depth_after_load = game.run().depth;
    game.headless.ticks(200);
    return game.world().state_hash();
}
} // namespace

TEST_CASE("a load issued as an intent inside a tick replays identically") {
    std::uint32_t depth_a = 0, depth_b = 0;
    const std::uint64_t a = play_with_tick_load(&depth_a);
    const std::uint64_t b = play_with_tick_load(&depth_b);
    CHECK(depth_a == depth_b);
    CHECK(a == b); // the F9 path is deterministic (a recorded intent replays the same load)
}

TEST_CASE("undo restores a turn exactly and replays identically") {
    Game game(9);
    game.headless.ticks(200);
    const std::uint64_t at_push = game.world().state_hash();
    lam::History history;
    history.push(game.world());
    game.headless.ticks(120);
    const std::uint64_t first = game.world().state_hash();
    REQUIRE(history.undo(game.world(), lam::RestoreOptions{.tick = true}));
    CHECK(game.world().state_hash() == at_push);
    game.headless.ticks(120);
    CHECK(game.world().state_hash() == first); // the bot and the rules are deterministic
}

TEST_CASE("a pack click during a step animation is used on the hero's next turn") {
    Game game(3, /*use_bot=*/false);
    game.headless.ticks(3);
    REQUIRE(game.run().phase == rl::Phase::Playing);
    lam::World &w = game.world();
    const lam::Entity hero = game.run().player;
    w.get<rl::Inventory>(hero).slots[0] = "potion_heal"_id;
    w.get<rl::Inventory>(hero).count = 1;
    w.get<rl::Stats>(hero).hp = 5;
    // The intent arrives in one tick only; make that tick a busy one: a real step animates, and
    // grid_movers (Animate) raises TurnScheduler::busy, and lowers it when the step ends. (A busy
    // flag set by hand with set_turns_busy would stay: grid_movers lowers only its own.)
    bool stepped = false;
    for (const lam::IVec2 dir :
         {lam::IVec2{1, 0}, lam::IVec2{-1, 0}, lam::IVec2{0, 1}, lam::IVec2{0, -1}})
        if (!stepped) stepped = lam::try_step(w, hero, dir) == lam::StepResult::Moved;
    REQUIRE(stepped);
    game.headless.ticks(1); // Animate: the step is under way, busy is set
    REQUIRE(w.resources().get<lam::TurnScheduler>().busy);
    game.headless.app().res<lam::InputSystem>().queue_intent(
        lam::Intent{.kind = rl::kUseItem, .a = 0});
    game.headless.ticks(1);
    CHECK(w.get<rl::Inventory>(hero).count == 1); // not the hero's turn yet: kept as pending_use
    CHECK(game.run().pending_use == 0);
    for (int i = 0; i < 10 && w.get<rl::Inventory>(hero).count == 1; ++i)
        game.headless.ticks(1);
    CHECK(w.get<rl::Inventory>(hero).count == 0);
    const rl::Stats &st = w.get<rl::Stats>(hero);
    CHECK(st.hp == 5 + 10 + st.max_hp / 4); // potion of healing: power 10 + a quarter of max hp
    CHECK(game.run().pending_use == -1);
}

// The pack's button workaround (game/view.cpp hud_input): focusable buttons whose focus is dropped
// every frame still take pointer clicks, but never key confirms (Space rests, Enter descends).
TEST_CASE("pack buttons: clicks work, keys never activate them") {
    lam::UiTree ui;
    const lam::UiId pack = ui.panel(0, lam::Rect{346, 14, 134, 0});
    const lam::UiId slot = ui.button(pack, "pack_1"_id, "1 healing potion");
    ui.layout(lam::Rect{0, 0, 480, 270}, lam::default_ui_theme(), nullptr);
    const lam::Rect r = ui.screen_rect(slot);
    REQUIRE(r.w > 0);
    const lam::Vec2 at{r.x + r.w / 2, r.y + r.h / 2};
    const auto clicked = [&] {
        for (const lam::UiEvent &e : ui.events())
            if (e.kind == lam::UiEvent::Kind::Clicked && e.id == "pack_1"_id) return true;
        return false;
    };
    const auto frame = [&](const lam::UiInput &in) {
        ui.clear_events();
        ui.handle_input(in);
        const bool c = clicked();
        ui.focus(0); // hud_input: drop the focus every frame
        return c;
    };
    // Keys: down, confirm, confirm again; nothing is ever activated.
    CHECK_FALSE(frame(lam::UiInput{.down = true}));
    CHECK_FALSE(frame(lam::UiInput{.confirm = true}));
    CHECK_FALSE(frame(lam::UiInput{.confirm = true}));
    // Pointer: press on one frame, release on the next (the focus is dropped in between).
    CHECK_FALSE(frame(lam::UiInput{
        .pointer = at, .pointer_valid = true, .pointer_down = true, .pointer_pressed = true}));
    CHECK(frame(lam::UiInput{.pointer = at, .pointer_valid = true, .pointer_released = true}));
}
