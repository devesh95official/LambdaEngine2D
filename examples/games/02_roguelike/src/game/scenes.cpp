#include <game/scenes.h>

#include <cstddef>
#include <cstdint>
#include <game/view.h>
#include <lam/core/fs.h>
#include <lam/core/log.h>
#include <lam/input/input_frame.h>
#include <lam/math/rng.h>
#include <lam/ui/menu_scenes.h>
#include <lam/ui/message_log.h>
#include <memory>
#include <optional>
#include <sim/controls.h>
#include <sim/run.h>
#include <sim/save.h>
#include <string>
#include <utility>

namespace game {
using namespace lam::literals;

void GameScene::enter(lam::App &app, lam::World &world) {
    rl::setup_run(app, world, seed_); // rules state, map, hero
    build_view(app, world);           // camera, fog style, HUD
    RunTracker &tracker = app.res<RunTracker>();
    tracker.world = &world;
    ++tracker.runs;
}

void GameScene::exit(lam::App &app, lam::World &world) {
    RunTracker &tracker = app.res<RunTracker>();
    if (tracker.world == &world) tracker.world = nullptr;
}

void GameScene::resumed(lam::App &app, const lam::SceneMessage *msg) {
    if (!msg) return;
    if (msg->kind == "new_run"_id) {
        const RunTracker &tracker = app.res<RunTracker>();
        app.res<lam::SceneStack>().replace(
            std::make_unique<GameScene>(lam::derive_seed(tracker.base_seed, tracker.runs)),
            lam::Transition{.kind = lam::TransitionKind::Fade, .ticks = 12});
    } else if (msg->kind == "quit"_id) {
        app.quit();
    }
}

namespace {
// Game-over menu items, in MenuSpec::items order (the menu's select intent carries this index).
constexpr std::size_t kNewRun = 0, kQuit = 1, kDeathMenuItems = 2;
} // namespace

lam::Intent new_run_intent() {
    return lam::Intent{.kind = lam::kMenuSelectIntent, .a = static_cast<std::int64_t>(kNewRun)};
}

std::optional<lam::SaveSlots> save_slots(const lam::App &app) {
    std::string dir = app.data_dir();
    if (dir.empty()) return std::nullopt;
    return lam::SaveSlots(std::move(dir), std::string(rl::kSaveGame), rl::kSaveVersion, 1);
}

void load_intent(lam::Ctx &ctx) {
    for (const lam::Intent &intent : lam::input_frame(ctx).intent_list()) {
        if (intent.kind != rl::kLoadGame) continue;
        lam::MessageLog *log = ctx.try_local<lam::MessageLog>();
        const std::optional<lam::SaveSlots> slots = save_slots(ctx.app());
        lam::Result<lam::SaveEnvelope> env =
            slots ? slots->load(1)
                  : lam::Result<lam::SaveEnvelope>(
                        lam::make_error(lam::ErrorCode::NotFound, "no data folder (headless)"));
        if (!env) {
            if (log) log->add("There is no saved game.", lam::Color{170, 170, 170, 255});
            return;
        }
        // A tick-stage load: deterministic under replay (the intent is recorded) and every
        // later system of this tick already sees the restored world.
        if (lam::Status s = rl::load_envelope(ctx.world(), *env, ctx.app().serialize_context());
            !s) {
            LAM_LOG_ERROR("roguelike: load failed: {}", s.error().message);
            if (log) log->add("The save is damaged.", lam::Color{255, 110, 100, 255});
        }
        return;
    }
}

void watch_death(lam::Ctx &ctx) {
    const rl::RunState &run = ctx.local<rl::RunState>();
    Hud &hud = ctx.local<Hud>();
    if (run.phase != rl::Phase::Dead || hud.game_over_shown) return;
    hud.game_over_shown = true;
    const rl::Stats *st = ctx.world().try_get<rl::Stats>(run.player);
    LAM_LOG_INFO("roguelike: run {} died on depth {} (level {}, {} kills, {} turns, score {})",
                 ctx.res<RunTracker>().runs, run.depth, st ? st->level : 0, run.kills, run.turns,
                 run.score);
    // Permadeath: this run's save dies with the hero (a save of another run is left alone).
    if (std::optional<lam::SaveSlots> slots = save_slots(ctx.app()))
        if (const lam::Result<lam::SaveEnvelope> env = slots->load(1); env)
            if (const lam::Result<std::uint64_t> seed = rl::saved_run_seed(*env);
                seed && *seed == run.run_seed)
                (void)slots->erase(1);
    lam::MenuSpec spec;
    spec.title = "You died";
    spec.back_pops = false;
    lam::FixedString<64> stats;
    lam::format_to(stats, "Depth {}   Kills {}   Score {}", run.depth, run.kills, run.score);
    spec.body = std::string(stats.view());
    spec.items.resize(kDeathMenuItems); // fixed indices: new_run_intent() refers to them
    spec.items[kNewRun] = lam::MenuItem{.id = "new_run"_id, .label = "New run"};
    spec.items[kQuit] = lam::MenuItem{.id = "quit"_id, .label = "Quit"};
    // Pushed from a tick system: takes effect at the next tick boundary, so it replays.
    ctx.res<lam::SceneStack>().push(
        std::make_unique<lam::MenuScene>(std::move(spec)),
        lam::Transition{.kind = lam::TransitionKind::Fade, .ticks = 12});
}
} // namespace game
