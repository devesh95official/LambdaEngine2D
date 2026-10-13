// The whole game minus presentation, run headless with the bot. Checks that the same seed gives
// the same state hash and that turns pass and monsters die or floors go by.
// TODO(tests): one TEST_CASE per rule you add (set up a World, step ticks, CHECK the state).
#include <cstdint>
#include <doctest/doctest.h>
#include <lam/core/default_plugins.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/turns.h>
#include <lam/input/driver.h>
#include <lam/test/support.h>
#include <game/rules.h>

namespace {
struct Outcome {
    std::uint64_t hash = 0, turns = 0;
    std::int32_t depth = 0, kills = 0;
};

Outcome play(std::uint64_t seed, int ticks) {
    game::BotDriver bot; // outlives the App
    lam::test::HeadlessApp headless({.seed = seed});
    lam::App &app = headless.app();
    lam::DefaultPlugins plugins;
    plugins.assets.root = GAME_ASSETS_DIR; // the game's assets/ folder (tests/CMakeLists.txt)
    app.add_plugins(plugins);
    app.add_plugin<game::RulesPlugin>();
    REQUIRE(app.res<lam::PrefabLibrary>().load("prefabs.json", app.serialize_context()).ok());
    game::start_run(app.main_world(), seed);
    app.res<lam::InputSystem>().set_driver(&bot);
    headless.ticks(ticks);
    app.res<lam::InputSystem>().set_driver(nullptr);
    lam::World &w = app.main_world();
    const game::RunState &run = w.resources().get<game::RunState>();
    return {w.state_hash(), w.resources().get<lam::TurnScheduler>().turns, run.depth, run.kills};
}
} // namespace

TEST_CASE("the bot plays the core loop, and the same seed replays the same run") {
    const Outcome a = play(7, 1800);
    const Outcome b = play(7, 1800);
    CHECK(a.hash == b.hash);
    CHECK(a.turns > 20);
    CHECK((a.depth >= 2 || a.kills >= 1));
}
