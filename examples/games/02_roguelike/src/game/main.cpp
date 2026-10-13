// Roguelike: a turn-based dungeon crawl on LambdaEngine2D (see README.md).
//   roguelike                       play (arrows/WASD, space, '.', 1-8, F5/F9)
//   roguelike --bot                 watch the InputDriver bot play
//   roguelike --headless --bot --frames 6000 --seed 7 [--hash-every 600]
//                                   no window; prints one "roguelike summary: ..." line at exit
#include <cinttypes>
#include <cstdio>
#include <game/scenes.h>
#include <game/view.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/log.h>
#include <lam/ecs/turns.h>
#include <lam/input/driver.h>
#include <lam/scene/scene.h>
#include <memory>
#include <sim/bot.h>
#include <sim/components.h>
#include <sim/data.h>
#include <sim/run.h>

namespace {
// One line with the same keys every run, so scripts can diff runs.
void print_summary(lam::App &app, const rl::BotDriver &bot) {
    const game::RunTracker &tracker = app.res<game::RunTracker>();
    const lam::World *w = tracker.world;
    const rl::RunState *run = w ? rl::run_state(*w) : nullptr;
    const rl::Stats *st =
        run && w->alive(run->player) ? w->try_get<rl::Stats>(run->player) : nullptr;
    const lam::TurnScheduler *turns = w ? w->resources().try_get<lam::TurnScheduler>() : nullptr;
    std::printf("roguelike summary: ticks=%" PRIu64 " score=%u depth=%u kills=%u turns=%" PRIu64
                " level=%u hp=%d dead=%d runs=%u bot_inputs=%u hash=%016" PRIx64 "\n",
                app.tick(), run ? run->score : 0u, run ? unsigned{run->depth} : 0u,
                run ? run->kills : 0u, turns ? turns->turns : std::uint64_t{0},
                st ? unsigned{st->level} : 0u, st ? int{st->hp} : 0,
                run && run->phase == rl::Phase::Dead ? 1 : 0, tracker.runs, bot.decisions(),
                w ? w->state_hash() : std::uint64_t{0});
}
} // namespace

int main(int argc, char **argv) {
    // The bot must outlive the App (InputSystem::set_driver does not take ownership).
    static rl::BotDriver bot;
    lam::App app({.title = "Roguelike", .virtual_size = {480, 270}, .seed = 20261012}, argc, argv);
    app.add_plugins(lam::DefaultPlugins{}); // backends are skipped under --headless
    app.add_plugin<rl::RulesPlugin>();      // rules (roguelike_sim)
    app.add_plugin<game::ViewPlugin>();     // presentation + flow (this executable)
    if (lam::Status s = rl::load_game_data(app); !s) {
        LAM_LOG_ERROR("roguelike: {}", s.error().message);
        return 1;
    }
    const bool bot_mode = app.options().has("--bot");
    if (bot_mode) {
        bot.set_restart_intent(game::new_run_intent()); // permadeath: start over from the menu
        app.res<lam::InputSystem>().set_driver(&bot);
    }
    app.res<game::RunTracker>().base_seed = app.seed();
    app.res<lam::SceneStack>().push(std::make_unique<game::GameScene>(app.seed()));
    const int code = app.run();
    if (app.options().headless && bot_mode) print_summary(app, bot);
    return code;
}
