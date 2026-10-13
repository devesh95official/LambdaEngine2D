// Pathar: stone puzzles of Shantipur, a Sokoban-like grid puzzle with water.
//   pathar                         title screen, menus, 24 levels in 3 chapters
//   pathar --level 9               straight into level 9
//   pathar --bot                   the solver plays every level in order (InputDriver)
//   pathar --headless --bot --frames 3000 --seed 1 [--hash-every 600]
//                                  no window; prints one "pathar summary: ..." line at exit
//   pathar --bot --bot-undo 7      the bot also presses Undo (and Restart + Undo) every 7 steps
// Wiring only: rules are in src/sim (pathar_sim, pure), presentation in src/game.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/log.h>
#include <lam/ecs/world.h>
#include <lam/input/driver.h>
#include <lam/scene/scene.h>
#include "game/game.h"
#include "game/look.h"
#include "game/scenes.h"
#include "sim/board.h"
#include "sim/bot.h"
#include "sim/sim_plugin.h"

namespace {

using namespace pathar;

// Startup system (once, before frame 1): the first scene.
void start(lam::Ctx &ctx) {
    const PatharGame &g = ctx.res<PatharGame>();
    lam::SceneStack &stack = ctx.res<lam::SceneStack>();
    if (g.bot || g.start_level >= 0) stack.push(make_play_scene(std::max(0, g.start_level)));
    else stack.push(make_title_scene());
}

// TickEnd Once: remember the focused puzzle world's hash for the summary (worlds die with scenes).
void bot_stats(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const PuzzleState *ps = w.resources().try_get<PuzzleState>();
    if (ps == nullptr) return;
    PatharGame &g = ctx.res<PatharGame>();
    g.last_level = ps->level;
    g.last_hash = w.state_hash();
}

int level_flag(const lam::LaunchOptions &o) {
    const int level = o.int_value("--level", 0);  // 1-based on the command line; 0 = absent
    return level > 0 ? level - 1 : -1;
}

}  // namespace

int main(int argc, char **argv) {
    lam::App app({.title = "Pathar", .virtual_size = {320, 180}}, argc, argv);
    app.add_plugins(lam::DefaultPlugins{.render = {.clear_color = lam::Color{20, 14, 12, 255}}});
    app.add_plugin<PatharSimPlugin>();
    const lam::LaunchOptions &opt = app.options();
    PatharGame &game = app.insert_resource<PatharGame>();
    game.bot = opt.has("--bot");
    game.scripted = opt.headless || game.bot || opt.frames > 0;
    game.start_level = level_flag(opt);
    if (lam::Status s = load_game(app, game); !s) {
        LAM_LOG_ERROR("pathar cannot start: {}", s.error().message);
        return 1;
    }
    app.add_plugin<PatharLookPlugin>();
    if (game.bot) {
        // The bot is an App resource so it outlives every tick; InputSystem only borrows it.
        PatharBot &bot = app.insert_resource<PatharBot>(app.res<Controls>(), opt.int_value("--bot-undo", 0));
        app.res<lam::InputSystem>().set_driver(&bot);
    }
    if (opt.headless && game.bot)
        app.add_system({.stage = lam::Stage::TickEnd, .fn = bot_stats, .name = "pathar.bot_stats", .order = 900,
                        .scope = lam::SystemScope::Once});
    app.add_startup(start, "pathar.start");
    const int code = app.run();
    if (opt.headless && game.bot) {
        std::printf("pathar summary: ticks=%" PRIu64 " score=%d solved=%d bot_solves=%d bot_moves=%lld level=%d "
                    "hash=%016" PRIx64 "\n",
                    app.tick(), game.progress.total_stars(), game.progress.solved_count(), game.bot_solves,
                    static_cast<long long>(game.bot_moves), game.last_level + 1, game.last_hash);
    }
    return code;
}
