// Shooter: top-down arena survival. Waves of hundreds of chasers (flow field + separation), mouse
// or twin-stick aim, prefab bullets, pickups, HUD, pause/game-over menus, and a self-playing bot.
//   shooter                               play (WASD + mouse, or a gamepad)
//   shooter --bot                         watch the bot play
//   shooter --headless --bot --frames 3600 --seed 7 [--hash-every 600]
//                                         no window; prints one "shooter summary:" line at exit
//   shooter --enemies 2000                stress/benchmark: every wave has N enemies, god mode
//   shooter --bot --hidden --no-vsync --frames 1200 --bench
//                                         prints one "shooter bench:" line (frame ms, fps, enemies)
//   shooter --headless --bot --frames 3600 --record-input run.lamr; shooter --headless
//           --frames 3600 --replay run.lamr  replays to the same summary hash
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/log.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/world.h>
#include <lam/input/driver.h>
#include <lam/scene/scene.h>
#include <shooter/arena.h>
#include <shooter/bot.h>
#include <shooter/components.h>
#include <shooter/sim_plugin.h>
#include "present/presentation.h"
#include "present/scenes.h"

using namespace lam;

namespace {
// The one-line machine-readable summary for --headless --bot runs.
void print_summary(App &app) {
    const shooter_app::Session &session = app.res<shooter_app::Session>();
    if (session.arena == nullptr) {
        std::printf("shooter summary: ticks=%" PRIu64
                    " score=0 wave=0 kills=0 total_kills=0 best=0 runs=0 deaths=0 enemies=0"
                    " hash=0\n",
                    app.tick());
        return;
    }
    World &w = *session.arena;
    const shooter::Arena &a = w.resources().get<shooter::Arena>();
    std::printf("shooter summary: ticks=%" PRIu64 " score=%" PRId64
                " wave=%u kills=%u total_kills=%u best=%" PRId64
                " runs=%u deaths=%u enemies=%zu hash=%016" PRIx64 "\n",
                app.tick(), a.score, a.wave, a.kills, a.total_kills, a.best, a.runs, a.deaths,
                w.count<shooter::Enemy>(), w.state_hash());
}
}

int main(int argc, char **argv) {
    shooter::ShooterBot bot;                                    // first: outlives the App
    App app({.title = "Shooter", .virtual_size = {480, 270}, .entity_capacity = 32768}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    const shooter_app::GameFlags flags = shooter_app::parse_flags(app.options());
    app.insert_resource<shooter_app::GameFlags>(flags);
    app.insert_resource<shooter_app::Session>();
    app.add_plugin<shooter::ShooterSimPlugin>();
    app.add_plugin<shooter_app::ShooterPresentationPlugin>();

    // Prefabs reference the sprite sheet by path; the AssetServer resolves it while loading.
    if (Status s = app.res<PrefabLibrary>().load("prefabs.json", app.serialize_context()); !s) {
        LAM_LOG_ERROR("prefabs.json: {}", s.error().message);
        return 1;
    }
    // --replay installs the engine's ReplayDriver in InputPlugin::build; the bot must not replace
    // it (a recorded --bot run replays without --bot, or with it).
    const bool replaying = !app.options().replay.empty();
    if (flags.bot && !replaying) app.res<InputSystem>().set_driver(&bot);
    app.res<SceneStack>().push(std::make_unique<shooter_app::ArenaScene>());

    const int code = app.run();
    if ((flags.bot || replaying) && app.options().headless) print_summary(app);
    shooter_app::print_bench(app);                              // --bench only
    return code;
}
