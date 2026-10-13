// Lantern Run: gather the lanterns lit in a forest at night while wolves hunt you (README.md).
//   rules         src/sim/   (lantern_run_sim: pure targets, tested headless in tests/)
//   presentation  src/view/  (camera, night lights, particles, HUD)
//   data          assets/prefabs.json (what actors look like), assets/levels/forest.txt (the map)
// Keys: WASD / arrows move, Space / Shift dash, Enter start, P / Esc pause.
// Flags: --bot (the Autopilot plays), plus the engine's --headless --frames N --seed S
// --hash-every N --step --record-input F --replay F --print-schedule.
// `--headless --bot` prints one summary line at exit.
// `--perf` prints frames, ticks and the mean frame time at exit (start-up excluded; Release checks).
#include <cinttypes>
#include <cstdio>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/log.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>

#include "sim/bot.h"
#include "sim/components.h"
#include "sim/controls.h"
#include "sim/plugin.h"
#include "view/view.h"

namespace {

void print_summary(lam::App &app) {
    lam::World &w = app.main_world();
    const lantern::RunState *run = w.resources().try_get<lantern::RunState>();
    const lantern::Player *player = w.single_or_null<lantern::Player>();
    if (run == nullptr) return;
    std::printf("lantern_run summary: ticks=%" PRIu64 " score=%d best=%d wave=%d hearts=%d wolves=%zu runs=%d "
                "deaths=%d hash=%016" PRIx64 "\n",
                w.tick(), run->collected, run->best, run->wave, player != nullptr ? player->hearts : 0,
                w.count<lantern::Wolf>(), run->runs, run->deaths, w.state_hash());
}

} // namespace

int main(int argc, char **argv) {
    // One seed (777, --seed overrides) drives every run.
    lam::App app({.title = "Lantern Run", .virtual_size = {480, 270}, .seed = 777, .audio = false}, argc, argv);
    const bool bot = app.options().has("--bot");

    lam::DefaultPlugins plugins;
    plugins.render.clear_color = {16, 18, 30, 255};
    plugins.render.lighting = true;                             // the "light" target for Light2D
    app.add_plugins(plugins);

    lantern::Controls controls;
    if (lam::ActionMap *map = app.try_res<lam::ActionMap>()) controls = lantern::bind_controls(*map);
    app.insert_resource<lantern::Controls>(controls);

    // Prefabs say what actors look like; the rules add their own components on top. Without them
    // the game still runs (bare entities), which keeps headless checks alive.
    if (lam::Status s = app.res<lam::PrefabLibrary>().load("prefabs.json", app.serialize_context()); !s) {
        LAM_LOG_ERROR("lantern_run: prefabs.json: {}", s.error().message);
    }

    app.add_plugin<lantern::LanternRunSimPlugin>(lantern::SimOptions{.start_playing = bot});
    app.add_plugin<lantern::LanternRunViewPlugin>();

    // The bot is an InputDriver: it fills player 0's InputFrame every tick (TickBegin -1400).
    lantern::Autopilot autopilot(controls);
    lam::InputSystem *input = app.try_res<lam::InputSystem>();
    if (bot && input != nullptr) input->set_driver(&autopilot);

    const int code = app.run();
    if (bot && app.options().headless) print_summary(app);
    if (app.options().has("--perf")) {
        const lam::Time &t = app.time();                        // real_seconds counts from the first frame
        const double ms = t.frame > 0 ? t.real_seconds * 1000.0 / static_cast<double>(t.frame) : 0.0;
        std::printf("lantern_run perf: frames=%" PRIu64 " ticks=%" PRIu64 " seconds=%.3f ms_per_frame=%.3f fps=%.0f\n",
                    t.frame, app.main_world().tick(), t.real_seconds, ms, ms > 0.0 ? 1000.0 / ms : 0.0);
    }
    if (input != nullptr) input->set_driver(nullptr);
    return code;
}
