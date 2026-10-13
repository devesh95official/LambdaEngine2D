#include "scenes.h"

#include <algorithm>
#include <format>
#include <memory>
#include <lam/input/input_frame.h>
#include "present.h"
#include "trail_rules.h"

namespace game {
namespace {

using trail::TrailState;

constexpr float kMidY = static_cast<float>(kVirtual.y) / 2 - 20;
constexpr float kMidX = static_cast<float>(kVirtual.x) / 2;

bool pressed(const lam::Ctx &ctx, lam::ButtonAction trail::Controls::*which) {
    return lam::input_frame(ctx).is_pressed(ctx.res<trail::Controls>().*which);
}

// ---- play world (global systems, run_if has_local<TrailState>) --------------------------------

// Simulate +100, after trail.step: pause requests and the bump -> game over hand-off.
void play_flow(lam::Ctx &ctx) {
    const TrailState &s = ctx.local<TrailState>();
    Session &session = ctx.res<Session>();
    lam::SceneStack &stack = ctx.res<lam::SceneStack>();
    session.best = std::max(session.best, s.score);
    session.last_score = s.score;
    session.last_apples = s.eaten;
    if (stack.queued() > 0) return;                             // one scene change per tick boundary
    if (!s.alive) {
        session.apples += s.eaten;
        session.total_score += s.score;
        ++session.crashes;
        stack.push(std::make_unique<OverScene>());              // stops this world ticking
        return;
    }
    if (pressed(ctx, &trail::Controls::pause)) stack.push(std::make_unique<PauseScene>());
}

// TickEnd +900: the play world's hash after every tick, for the headless summary line.
void record_hash(lam::Ctx &ctx) {
    Session &session = ctx.res<Session>();
    session.last_hash = ctx.world().state_hash();
    session.ticks = ctx.time().app_tick;
}

// ---- overlay scenes (Scene::add_system: these live and die with the scene's world) -------------

void title_tick(lam::Ctx &ctx) {
    if (pressed(ctx, &trail::Controls::confirm) && ctx.res<lam::SceneStack>().queued() == 0)
        ctx.res<lam::SceneStack>().pop();
}
void title_draw(lam::Ctx &ctx) {
    draw_dim(ctx, {0, 0, 0, 120});
    draw_label(ctx, "APPLE TRAIL", {kMidX, kMidY - 10}, 16, kGold, true);
    draw_label(ctx, "Pick apples. Each one joins your trail.", {kMidX, kMidY + 18}, 8, kHudInk, true);
    draw_label(ctx, "Don't bump the walls or your own trail.", {kMidX, kMidY + 30}, 8, kHudInk, true);
    draw_label(ctx, "ARROWS / WASD to turn  -  ENTER to start", {kMidX, kMidY + 50}, 8, kHudInk, true);
}

void pause_tick(lam::Ctx &ctx) {
    if ((pressed(ctx, &trail::Controls::pause) || pressed(ctx, &trail::Controls::confirm)) &&
        ctx.res<lam::SceneStack>().queued() == 0)
        ctx.res<lam::SceneStack>().pop();
}
void pause_draw(lam::Ctx &ctx) {
    draw_dim(ctx, {0, 0, 0, 120});
    draw_label(ctx, "PAUSED", {kMidX, kMidY}, 16, kHudInk, true);
    draw_label(ctx, "P or ENTER to go on", {kMidX, kMidY + 26}, 8, kHudInk, true);
}

void over_tick(lam::Ctx &ctx) {
    if (pressed(ctx, &trail::Controls::confirm) && ctx.res<lam::SceneStack>().queued() == 0)
        ctx.res<lam::SceneStack>().reset(std::make_unique<PlayScene>());   // a fresh world per run
}
void over_draw(lam::Ctx &ctx) {
    const Session &session = ctx.res<Session>();
    draw_dim(ctx, {0, 0, 0, 120});
    draw_label(ctx, "BUMP!", {kMidX, kMidY - 4}, 16, kBump, true);
    char buf[48];
    const auto r = std::format_to_n(buf, sizeof buf, "Score {}   Apples {}", session.last_score,
                                    session.last_apples);
    draw_label(ctx, std::string_view(buf, static_cast<std::size_t>(r.out - buf)), {kMidX, kMidY + 22}, 8, kHudInk, true);
    draw_label(ctx, "ENTER to play again", {kMidX, kMidY + 38}, 8, kHudInk, true);
}

} // namespace

void PlayScene::enter(lam::App &app, lam::World &world) {
    trail::start_run(world, app.res<GameArt>().orchard);
    dress_run(app, world);
    ++app.res<Session>().runs;
}

void TitleScene::enter(lam::App &app, lam::World &) {
    add_system(app, lam::Stage::Simulate, title_tick, "title.tick");
    add_system(app, lam::Stage::RenderUI, title_draw, "title.draw");
}

void PauseScene::enter(lam::App &app, lam::World &) {
    add_system(app, lam::Stage::Simulate, pause_tick, "pause.tick");
    add_system(app, lam::Stage::RenderUI, pause_draw, "pause.draw");
}

void OverScene::enter(lam::App &app, lam::World &) {
    add_system(app, lam::Stage::Simulate, over_tick, "over.tick");
    add_system(app, lam::Stage::RenderUI, over_draw, "over.draw");
}

void FlowPlugin::build(lam::App &app) {
    app.require_plugin("lam.scene", name());
    app.add_system({.stage = lam::Stage::Simulate, .fn = play_flow, .name = "flow.play", .order = 100,
                    .run_if = lam::has_local<TrailState>});
    app.add_system({.stage = lam::Stage::TickEnd, .fn = record_hash, .name = "flow.hash", .order = 900,
                    .run_if = lam::has_local<TrailState>});
}

} // namespace game
