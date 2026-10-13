// Pong: window, drawing, HUD and particles. The rules are in src/pong/.
//   pong --bot                            watch the bot play the CPU
//   pong --headless --bot --frames 3000   no window, prints a summary line
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/fixed_string.h>
#include <lam/ecs/world.h>
#include <lam/math/color.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <pong/pong.h>

using namespace lam;

namespace {
// Palette (Sweetie 16).
const Color kBg = Color::hex(0x1a1c2cff), kField = Color::hex(0x222538ff), kLine = Color::hex(0x333c57ff);
const Color kBlue = Color::hex(0x41a6f6ff), kOrange = Color::hex(0xef7d57ff), kBallC = Color::hex(0xf4f4f4ff);
const Color kGold = Color::hex(0xffcd75ff), kDim = Color::hex(0x566c86ff);

float lerp(float a, float b, float t) { return a + (b - a) * t; }

void setup(Ctx &ctx) {
    World &w = ctx.world();
    pong::start(w);
    const Entity cam = w.create();                              // view only
    w.add<Camera2D>(cam, Camera2D{.position = {pong::kW / 2, pong::kH / 2}});
    w.add<CameraShake>(cam, CameraShake{.max_offset = 5});
}

void text(Ctx &ctx, std::string_view s, float x, float y, float size, Color c, bool centred = true) {
    const Font *font = ctx.res<AssetServer>().get(ctx.res<RenderDefaults>().font);
    if (font == nullptr) return;
    if (centred) x -= measure_text(*font, s, size).x / 2;
    draw_text(ctx.res<RenderQueue>(), *font, s, {x, y},
              TextStyle{.size = size, .color = c, .layer = kLayerUi, .shadow = Color{0, 0, 0, 160}});
}

// Field, paddles and ball go on world layers so the camera shake moves them.
void draw_field(Ctx &ctx) {
    using namespace pong;
    const Match &m = ctx.local<Match>();
    RenderQueue &q = ctx.res<RenderQueue>();
    const float a = ctx.time().alpha;
    q.rect(Rect{-20, -20, kW + 40, kH + 40}, kBg, kLayerBackground);
    q.rect(Rect{0, kTop, kW, kBottom - kTop}, kField, kLayerBackground, 1);
    q.rect(Rect{0, kTop - 3, kW, 3}, kLine, kLayerTiles);
    q.rect(Rect{0, kBottom, kW, 3}, kLine, kLayerTiles);
    for (float y = kTop + 4; y < kBottom - 4; y += 14) q.rect(Rect{kW / 2 - 1, y, 2, 8}, kLine, kLayerTiles);
    q.circle({kW / 2, (kTop + kBottom) / 2}, 26, kLine, kLayerTiles);
    q.circle({kW / 2, (kTop + kBottom) / 2}, 24, kField, kLayerTiles, 1);
    const float p1 = lerp(m.prev_p1, m.p1, a), p2 = lerp(m.prev_p2, m.p2, a);
    q.rect(Rect{kPadInset, p1, kPadW, kPadH}, kBlue, kLayerWorld);
    q.rect(Rect{kW - kPadInset - kPadW, p2, kPadW, kPadH}, kOrange, kLayerWorld);
    if (m.phase == kTitle || m.phase == kOver) return;
    if (m.phase == kServe && (m.phase_ticks / 8) % 2 == 1) return;   // blink before the serve
    const Vec2 b{lerp(m.prev_ball.x, m.ball.x, a), lerp(m.prev_ball.y, m.ball.y, a)};
    for (int i = 3; i >= 1; --i) {                              // speed trail
        const Vec2 g = b - m.vel * (1.6f * static_cast<float>(i));
        const auto alpha = static_cast<std::uint8_t>(110 - 30 * i);
        q.rect(Rect{g.x + 1, g.y + 1, kBall - 2, kBall - 2}, kGold.with_alpha(alpha), kLayerWorld);
    }
    q.rect(Rect{b.x, b.y, kBall, kBall}, kBallC, kLayerWorld, 1);
}

// HUD and the title / game-over cards, in screen space.
void draw_ui(Ctx &ctx) {
    using namespace pong;
    const Match &m = ctx.local<Match>();
    RenderQueue &q = ctx.res<RenderQueue>();
    FixedString<64> s;
    text(ctx, "PLAYER 1", 8, 7, 8, kBlue, false);
    text(ctx, m.two_player ? "PLAYER 2" : "CPU", kW - 8 - (m.two_player ? 48.0f : 18.0f), 7, 8, kOrange, false);
    format_to(s, "RALLY {}   BEST {}   WINS {}", m.rally, m.best_rally, m.wins);
    text(ctx, s.view(), kW / 2, 7, 8, kDim);
    if (m.phase == kServe || m.phase == kPlay) {
        s.clear();
        format_to(s, "{}", m.score1);
        text(ctx, s.view(), kW / 2 - 40, kTop + 10, 32, kBlue.with_alpha(200));
        s.clear();
        format_to(s, "{}", m.score2);
        text(ctx, s.view(), kW / 2 + 40, kTop + 10, 32, kOrange.with_alpha(200));
        return;
    }
    q.rect(Rect{90, 46, 300, 190}, Color{14, 15, 26, 225}, kLayerUi);
    q.rect_lines(Rect{90, 46, 300, 190}, 2, m.phase == kTitle ? kGold : (m.winner == 1 ? kBlue : kOrange), kLayerUi);
    if (m.phase == kTitle) {
        text(ctx, "PONG", kW / 2, 62, 48, kGold);
        text(ctx, "FIRST TO 7", kW / 2, 116, 8, kDim);
        text(ctx, m.two_player ? "  1 PLAYER VS CPU" : "> 1 PLAYER VS CPU", kW / 2, 136, 8, m.two_player ? kDim : kBallC);
        text(ctx, m.two_player ? "> 2 PLAYERS      " : "  2 PLAYERS      ", kW / 2, 150, 8, m.two_player ? kBallC : kDim);
        text(ctx, "TAB / W / S  switch mode", kW / 2, 174, 8, kDim);
        text(ctx, "P1: W S (or mouse)   P2: UP DOWN", kW / 2, 188, 8, kDim);
        text(ctx, "SPACE / CLICK TO START", kW / 2, 210, 8, (m.phase_ticks / 30) % 2 ? kGold : kBallC);
        return;
    }
    const bool cpu = m.winner == 2 && !m.two_player;
    text(ctx, cpu ? "CPU WINS" : (m.winner == 1 ? "PLAYER 1 WINS" : "PLAYER 2 WINS"), kW / 2, 70, 24,
         m.winner == 1 ? kBlue : kOrange);
    s.clear();
    format_to(s, "{} - {}", m.score1, m.score2);
    text(ctx, s.view(), kW / 2, 110, 24, kBallC);
    s.clear();
    format_to(s, "BEST RALLY {}    WINS VS CPU {}", m.best_rally, m.wins);
    text(ctx, s.view(), kW / 2, 156, 8, kGold);
    text(ctx, "SPACE / CLICK  rematch", kW / 2, 186, 8, kBallC);
    text(ctx, "T / TAB  title", kW / 2, 200, 8, kDim);
}

void load_fx(App &app) {
    AssetServer &assets = app.res<AssetServer>();
    const auto spark = [](Color c) {
        return sanitize(EmitterConfig{.capacity = 512, .speed_min = 40, .speed_max = 140, .life_min = 0.2f,
                                      .life_max = 0.5f, .size_start = 3, .color_start = c,
                                      .color_end = c.with_alpha(0), .drag = 3});
    };
    app.insert_resource<pong::Fx>(pong::Fx{.hit1 = assets.add(spark(kBlue), "fx/hit1"),
                                           .hit2 = assets.add(spark(kOrange), "fx/hit2"),
                                           .goal = assets.add(spark(kGold), "fx/goal")});
}
}

int main(int argc, char **argv) {
    pong::Bot bot;                                              // outlives the App
    App app({.title = "Pong", .virtual_size = {480, 270}}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.add_plugin<pong::RulesPlugin>();
    load_fx(app);
    app.add_startup(setup, "pong.setup");
    app.add_system({.stage = Stage::Render, .fn = draw_field, .name = "pong.draw_field",
                    .run_if = &has_local<pong::Match>});
    app.add_system({.stage = Stage::RenderUI, .fn = draw_ui, .name = "pong.draw_ui",
                    .run_if = &has_local<pong::Match>});
    if (app.options().has("--bot")) app.res<InputSystem>().set_driver(&bot);
    const int code = app.run();
    if (app.options().headless) {
        World &w = app.main_world();
        const pong::Match *mp = w.resources().try_get<pong::Match>();
        const pong::Match m = mp ? *mp : pong::Match{};
        std::printf("pong summary: score=%u cpu=%u match=%d-%d matches=%u wins=%u hits=%u best_rally=%u"
                    " ticks=%" PRIu64 " hash=%016" PRIx64 "\n",
                    m.points1, m.points2, m.score1, m.score2, m.matches, m.wins, m.hits, m.best_rally,
                    app.tick(), w.state_hash());
    }
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
