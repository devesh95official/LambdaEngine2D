// Breakout: paddle, ball, a wall of bricks, score and lives; shapes only (no asset files).
// Written from docs/RECIPES.md as a check that the recipes are enough for a small game.
//   breakout                          play (A/D or arrows, Space to launch / restart)
//   breakout --headless --bot --frames 3000 --seed 7   prints one summary line
#include <cstdio>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/fixed_string.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/color.h>
#include <lam/physics/components.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include <breakout/breakout.h>

using namespace lam;
using namespace breakout;

namespace {
void start(Ctx &ctx) { start_game(ctx.world()); }

// Stage::Render: world pixels (no camera: world pixels are virtual pixels).
void draw_world(Ctx &ctx) {
    static constexpr Color kRowColors[kRows] = {{230, 70, 80, 255}, {240, 150, 60, 255}, {240, 210, 70, 255},
                                                {90, 200, 110, 255}, {80, 150, 240, 255}};
    RenderQueue &q = ctx.res<RenderQueue>();
    World &w = ctx.world();
    const float alpha = ctx.time().alpha;
    q.rect(Rect{0, 0, kFieldW, kFieldH}, Color{14, 16, 30, 255}, kLayerBackground);
    for (auto [e, tf, col] : w.view<Transform, Collider>(With<Wall>{}))
        q.rect(collider_box(col, tf.position), Color{70, 76, 104, 255}, kLayerTiles);
    for (auto [e, tf, col, brick] : w.view<Transform, Collider, Brick>()) {
        const Rect r = collider_box(col, tf.position);
        q.rect(r, kRowColors[brick.row % kRows], kLayerWorld);
        q.rect(Rect{r.x, r.y, r.w, 2}, Color{255, 255, 255, 70}, kLayerWorld, 1);   // highlight
    }
    for (auto [e, tf, col] : w.view<Transform, Collider>(With<Paddle>{}))
        q.rect(collider_box(col, interpolated(w, e, alpha).position), Color{220, 225, 240, 255}, kLayerWorld);
    for (auto [e, tf] : w.view<Transform>(With<Ball>{}))
        q.circle(interpolated(w, e, alpha).position, kBallRadius, Color{255, 255, 255, 255}, kLayerForeground);
}

// Stage::RenderUI: score line and prompts (virtual pixels).
void draw_hud(Ctx &ctx) {
    const Font *font = ctx.res<AssetServer>().get(ctx.res<RenderDefaults>().font);
    if (font == nullptr) return;
    RenderQueue &q = ctx.res<RenderQueue>();
    const Run &run = ctx.local<Run>();
    FixedString<64> text;
    format_to(text, "SCORE {}   LIVES {}   LEVEL {}", run.score, run.lives, run.level);
    draw_text(q, *font, text.view(), {10, 0}, TextStyle{.size = 8});
    if (run.over) {
        draw_text(q, *font, "GAME OVER", {133, 130}, TextStyle{.size = 8});
        draw_text(q, *font, "SPACE TO RESTART", {112, 142}, TextStyle{.size = 8});
    } else if (run.serving) {
        draw_text(q, *font, "SPACE TO LAUNCH", {115, 150}, TextStyle{.size = 8});
    }
}
}

int main(int argc, char **argv) {
    Bot bot;                                                    // declared first: outlives the App
    App app({.title = "Breakout", .window_size = {960, 720}, .virtual_size = {320, 240}}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.add_plugin<RulesPlugin>();
    app.add_startup(start, "breakout.start");
    app.add_system({.stage = Stage::Render, .fn = draw_world, .name = "breakout.draw", .run_if = &has_local<Run>});
    app.add_system({.stage = Stage::RenderUI, .fn = draw_hud, .name = "breakout.hud", .run_if = &has_local<Run>});
    if (app.options().has("--bot")) {
        bot.controls = app.res<Controls>();
        app.res<InputSystem>().set_driver(&bot);
    }
    const int code = app.run();
    if (app.options().headless) {
        const World &w = app.main_world();
        const Run *run = w.resources().try_get<Run>();
        const Stats *stats = w.resources().try_get<Stats>();
        Vec2 ball;
        for (auto [e, tf] : app.main_world().view<Transform>(With<Ball>{})) ball = tf.position;
        if (run != nullptr && stats != nullptr)
            std::printf("breakout summary: ticks=%llu score=%d lives=%d level=%d bricks_left=%d bricks_broken=%d "
                        "paddle_hits=%d balls_lost=%d over=%d games=%d best=%d total_bricks=%d ball=(%.0f,%.0f) "
                        "hash=%016llx\n",
                        static_cast<unsigned long long>(app.tick()), run->score, run->lives, run->level,
                        run->bricks_left, run->bricks_broken, run->paddle_hits, run->balls_lost, run->over ? 1 : 0,
                        stats->games, stats->best_score, stats->total_bricks, static_cast<double>(ball.x),
                        static_cast<double>(ball.y), static_cast<unsigned long long>(app.main_world().state_hash()));
    }
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
