// Same App as `top_down_shooter --headless --bot` minus presentation. The bot plays; the test
// checks it scores and that the run is deterministic. TODO(tests): one case per rule you add.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <lam/core/default_plugins.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/world.h>
#include <lam/input/driver.h>
#include <lam/test/support.h>
#include <game/game.h>

namespace {
struct Game {
    game::Bot bot;                                              // first: outlives the App
    lam::test::HeadlessApp h;
    explicit Game(std::uint64_t seed) : h(lam::AppConfig{.seed = seed}) {
        lam::App &app = h.app();
        const char *src = std::getenv("LAM_TEST_SOURCE_DIR");   // set by lam_add_test
        const std::string assets = std::string(src ? src : ".") + "/../assets";
        app.add_plugins(
            lam::DefaultPlugins{.headless = true, .scenes = false, .assets = {.root = assets}});
        app.add_plugin<game::RulesPlugin>();
        const lam::Status s =
            app.res<lam::PrefabLibrary>().load("prefabs.json", app.serialize_context());
        REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
        game::start_run(app, app.main_world());
        app.res<lam::InputSystem>().set_driver(&bot);
    }
    lam::World &world() { return h.app().main_world(); }
    const game::Run &run() { return world().resources().get<game::Run>(); }
};
}

TEST_CASE("the bot shoots enemies for points, and the same seed plays the same game") {
    Game a(7), b(7);
    a.h.ticks(60 * 40);                                         // 40 s of game time
    b.h.ticks(60 * 40);
    INFO("score " << a.run().score << " kills " << a.run().kills << " deaths " << a.run().deaths);
    CHECK(a.run().kills > 0);
    CHECK(std::max(a.run().score, a.run().best) > 0);           // best keeps points over a restart
    CHECK(a.world().state_hash() == b.world().state_hash());
}
