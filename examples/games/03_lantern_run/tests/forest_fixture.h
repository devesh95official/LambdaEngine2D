#pragma once
// A headless App running the Lantern Run rules on the forest level. `engine` adds the default
// engine plugins (headless: physics, tilemap, input... but no window), which the motors, the A*
// service and the bot need; without it only the sim plugin runs (ECS + rules, no movement).
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <lam/core/default_plugins.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/test/support.h>

#include "sim/bot.h"
#include "sim/components.h"
#include "sim/controls.h"
#include "sim/plugin.h"

namespace lantern::test {

inline std::string forest_text() {
    std::ifstream f(LANTERN_ASSETS "/levels/forest.txt");
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Forest {
    std::unique_ptr<Autopilot> bot;                             // destroyed after the App (declared first)
    lam::test::HeadlessApp h;

    explicit Forest(std::uint64_t seed = 777, bool engine = false, bool playing = true)
        : h(lam::AppConfig{.seed = seed, .audio = false}) {
        if (engine) h.app().add_plugins(lam::DefaultPlugins{.headless = true});
        h.add<LanternRunSimPlugin>(SimOptions{.level_text = forest_text(), .start_playing = playing});
    }
    // Binds the controls and lets the Autopilot drive player 0 (needs `engine`).
    void use_bot() {
        lam::App &app = h.app();
        if (app.try_res<Controls>() == nullptr) app.insert_resource<Controls>(bind_controls(app.res<lam::ActionMap>()));
        const Controls c = app.res<Controls>();
        bot = std::make_unique<Autopilot>(c);
        app.res<lam::InputSystem>().set_driver(bot.get());
    }
    lam::World &world() { return h.app().main_world(); }
    RunState &run() { return world().resources().get<RunState>(); }
    lam::Entity player() {
        lam::Entity e;
        world().single_or_null<Player>(&e);
        return e;
    }
};

} // namespace lantern::test
