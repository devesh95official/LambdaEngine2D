// Dino Runner: drawing, HUD and effects. Rules are in src/dino/.
//   dino --bot                            watch the bot
//   dino --headless --bot --frames 3000   no window, prints a summary line
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/keys.h>
#include <lam/math/color.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/render/shapes.h>
#include <lam/render/sprite.h>
#include "dino/rules.h"

using namespace lam;
using dino::DinoState;
using dino::kTune;
using dino::Phase;

namespace {

constexpr std::uint8_t kLayerSky = kLayerBackground, kLayerHills = 8, kLayerGround = kLayerTiles, kLayerActors = 100;

// Day and night colours, blended by night_amount.
struct Palette { Color sky_top, sky_low, hill_far, hill_near, ground, ground_line, speck, ink, cloud; };
constexpr Palette kDay{{126, 200, 227, 255}, {250, 236, 205, 255}, {178, 205, 160, 255}, {222, 196, 132, 255},
                       {236, 205, 140, 255}, {170, 130, 80, 255}, {196, 158, 100, 255}, {38, 70, 83, 255},
                       {255, 255, 255, 230}};
constexpr Palette kNight{{16, 24, 52, 255}, {58, 58, 96, 255}, {40, 50, 82, 255}, {74, 70, 104, 255},
                         {92, 86, 118, 255}, {58, 52, 84, 255}, {120, 112, 150, 255}, {241, 250, 238, 255},
                         {110, 116, 150, 160}};
constexpr Color kDino{42, 157, 143, 255}, kDinoDark{28, 112, 104, 255}, kWhite_{255, 255, 255, 255};
constexpr Color kCactus{88, 129, 87, 255}, kCactusLight{120, 166, 110, 255}, kBird{231, 111, 81, 255};
constexpr Color kBirdDark{180, 78, 58, 255}, kSun{244, 162, 97, 255}, kMoon{241, 250, 238, 255};

float smooth(float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3 - 2 * t); }
float wrap(float v, float span) { v = std::fmod(v, span); return v < 0 ? v + span : v; }

// 0 is day, 1 is night. One cycle per 700 points.
float night_amount(float score) {
    const float p = wrap(score / 700.0f, 1.0f);
    if (p < 0.45f) return 0;
    if (p < 0.55f) return smooth((p - 0.45f) / 0.1f);
    if (p < 0.9f) return 1;
    return 1 - smooth((p - 0.9f) / 0.1f);
}

Palette mix(float n) {
    const auto l = [n](Color a, Color b) { return lerp(a, b, n); };
    return {l(kDay.sky_top, kNight.sky_top), l(kDay.sky_low, kNight.sky_low), l(kDay.hill_far, kNight.hill_far),
            l(kDay.hill_near, kNight.hill_near), l(kDay.ground, kNight.ground), l(kDay.ground_line, kNight.ground_line),
            l(kDay.speck, kNight.speck), l(kDay.ink, kNight.ink), l(kDay.cloud, kNight.cloud)};
}

Color with_alpha(Color c, float a) { c.a = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 1.0f) * c.a); return c; }

// Scroll position interpolated between ticks.
float view_distance(const Ctx &ctx, const DinoState &s) {
    return s.phase == Phase::Play ? s.distance - s.speed * (1 - ctx.time().alpha) : s.distance;
}

void draw_cactus(RenderQueue &q, Vec2 b, float h, float tw, std::int32_t variant) {
    q.rect({b.x - tw / 2, b.y - h + tw / 2, tw, h - tw / 2}, kCactus, kLayerActors);
    q.circle({b.x, b.y - h + tw / 2}, tw / 2, kCactus, kLayerActors);
    q.rect({b.x - tw / 2 + 1, b.y - h + tw / 2, 1, h - tw / 2 - 2}, kCactusLight, kLayerActors);
    const float la = h * (variant % 2 == 0 ? 0.55f : 0.45f), ra = h * (variant < 2 ? 0.7f : 0.6f);
    q.rect({b.x - tw / 2 - 4, b.y - la, 5, 3}, kCactus, kLayerActors);           // left arm
    q.rect({b.x - tw / 2 - 4, b.y - la - h * 0.28f, 3, h * 0.28f + 3}, kCactus, kLayerActors);
    q.rect({b.x + tw / 2 - 1, b.y - ra, 5, 3}, kCactus, kLayerActors);           // right arm
    q.rect({b.x + tw / 2 + 1, b.y - ra - h * 0.22f, 3, h * 0.22f + 3}, kCactus, kLayerActors);
}

void draw_obstacle(RenderQueue &q, Vec2 b, const dino::Obstacle &o, std::uint32_t ticks) {
    using dino::Kind;
    switch (o.kind) {
    case Kind::SmallCactus: draw_cactus(q, b, o.h, 6, o.variant); break;
    case Kind::BigCactus: draw_cactus(q, b, o.h, 8, o.variant); break;
    case Kind::Cluster:
        draw_cactus(q, b + Vec2{-9, 0}, 18, 5, 1);
        draw_cactus(q, b + Vec2{9, 0}, 20, 5, 2);
        draw_cactus(q, b, o.h, 6, o.variant);
        break;
    default: {                                                  // bird, facing left
        const Vec2 c{b.x + 2, b.y - 6};
        const bool up = (ticks / 9) % 2 == 0;
        q.ellipse(c, {8, 4}, kBird, kLayerActors);
        q.circle({c.x - 8, c.y - 2}, 3.5f, kBird, kLayerActors);
        draw_triangle(q, {c.x - 11, c.y - 3}, {c.x - 17, c.y - 1}, {c.x - 11, c.y}, kSun, kLayerActors);
        q.rect({c.x - 9, c.y - 4, 1, 1}, kNight.sky_top, kLayerActors);
        draw_triangle(q, {c.x + 7, c.y - 1}, {c.x + 12, c.y - 4}, {c.x + 12, c.y + 1}, kBirdDark, kLayerActors);
        if (up) draw_triangle(q, {c.x - 3, c.y - 2}, {c.x + 5, c.y - 2}, {c.x + 3, c.y - 12}, kBirdDark, kLayerActors);
        else draw_triangle(q, {c.x - 3, c.y + 1}, {c.x + 5, c.y + 1}, {c.x + 3, c.y + 10}, kBirdDark, kLayerActors);
    }
    }
}

void draw_runner(RenderQueue &q, Vec2 f, const dino::Runner &r, bool dead, float dist, Color ink) {
    const int stride = static_cast<int>(dist / 14.0f) % 2;
    const auto eye = [&](Vec2 at) {
        if (dead) {
            q.line(at + Vec2{-1, -1}, at + Vec2{2, 2}, 1, ink, kLayerActors);
            q.line(at + Vec2{2, -1}, at + Vec2{-1, 2}, 1, ink, kLayerActors);
        } else {
            q.rect({at.x - 1, at.y - 1, 3, 3}, kWhite_, kLayerActors);
            q.rect({at.x, at.y - 1, 1, 2}, kDay.ink, kLayerActors);
        }
    };
    if (r.ducking) {
        q.rect({f.x - 11, f.y - 11, 19, 7}, kDino, kLayerActors);
        q.rect({f.x + 5, f.y - 13, 11, 7}, kDino, kLayerActors);
        draw_triangle(q, {f.x - 11, f.y - 10}, {f.x - 18, f.y - 10}, {f.x - 11, f.y - 6}, kDinoDark, kLayerActors);
        eye({f.x + 11, f.y - 11});
        q.rect({f.x - 6, f.y - 4, 3, stride ? 4.0f : 2.0f}, kDinoDark, kLayerActors);
        q.rect({f.x + 1, f.y - 4, 3, stride ? 2.0f : 4.0f}, kDinoDark, kLayerActors);
        return;
    }
    q.rect({f.x - 8, f.y - 17, 13, 10}, kDino, kLayerActors);                    // body
    q.rect({f.x + 1, f.y - 25, 11, 8}, kDino, kLayerActors);                     // head
    q.rect({f.x + 7, f.y - 19, 5, 1}, kDinoDark, kLayerActors);                  // mouth
    draw_triangle(q, {f.x - 8, f.y - 16}, {f.x - 15, f.y - 19}, {f.x - 8, f.y - 10}, kDinoDark, kLayerActors);
    q.rect({f.x + 5, f.y - 12, 3, 2}, kDinoDark, kLayerActors);                  // arm
    q.rect({f.x - 6, f.y - 9, 9, 2}, kDinoDark, kLayerActors);                   // belly line
    eye({f.x + 7, f.y - 22});
    const bool run = r.grounded && !dead;
    q.rect({f.x - 5, f.y - 7, 3, run && stride ? 4.0f : 7.0f}, kDinoDark, kLayerActors);
    q.rect({f.x + 1, f.y - 7, 3, run && !stride ? 4.0f : 7.0f}, kDinoDark, kLayerActors);
}

// World layers, shaken by the camera.
void draw_world(Ctx &ctx) {
    World &w = ctx.world();
    const DinoState &s = ctx.local<DinoState>();
    RenderQueue &q = ctx.res<RenderQueue>();
    const float dist = view_distance(ctx, s);
    const float n = night_amount(dist * 0.04f);
    const Palette p = mix(n);
    const float gy = kTune.ground_y;

    constexpr int kBands = 8;                                   // sky gradient
    for (int i = 0; i < kBands; ++i) {
        const float t = static_cast<float>(i) / (kBands - 1);
        const float y0 = -16 + (gy + 16) * static_cast<float>(i) / kBands;
        q.rect({-16, y0, 512, (gy + 16) / kBands + 1}, lerp(p.sky_top, p.sky_low, t), kLayerSky);
    }
    if (n > 0.01f) {                                            // stars
        for (std::uint32_t i = 0; i < 46; ++i) {
            const std::uint32_t h = i * 2654435761U;
            const float x = wrap(static_cast<float>(h % 997U) * 0.52f - dist * 0.01f, 500) - 10;
            const float y = static_cast<float>((h >> 12) % 140U) + 4;
            const float tw = 0.6f + 0.4f * std::sin(static_cast<float>(ctx.time().real_seconds) * 2 + static_cast<float>(i));
            q.rect({x, y, i % 5 == 0 ? 2.0f : 1.0f, i % 5 == 0 ? 2.0f : 1.0f}, with_alpha(kMoon, n * tw), kLayerSky);
        }
    }
    q.circle({392, 52 + n * 150}, 16, with_alpha(kSun, 0.35f * (1 - n)), kLayerSky);   // sun sets
    q.circle({392, 52 + n * 150}, 12, kSun, kLayerSky);
    const Vec2 moon{96, 190 - n * 140};                         // moon rises
    q.circle(moon, 10, with_alpha(kMoon, n), kLayerSky);
    q.circle(moon + Vec2{5, -3}, 9, with_alpha(p.sky_top, n), kLayerSky);
    for (int i = 0; i < 4; ++i) {                               // clouds
        const float x = wrap(static_cast<float>(i) * 150 + 60 - dist * 0.12f, 600) - 60;
        const float y = 40 + static_cast<float>((i * 37) % 60);
        q.ellipse({x, y}, {18, 6}, p.cloud, kLayerSky);
        q.ellipse({x + 10, y - 4}, {12, 7}, p.cloud, kLayerSky);
        q.ellipse({x - 9, y - 2}, {9, 5}, p.cloud, kLayerSky);
    }
    for (int i = 0; i < 5; ++i) {                               // far hills, slow parallax
        const float x = wrap(static_cast<float>(i) * 140 - dist * 0.2f, 700) - 110;
        q.ellipse({x, gy + 6}, {100, 48 + static_cast<float>(i % 3) * 10}, p.hill_far, kLayerHills);
    }
    for (int i = 0; i < 6; ++i) {                               // near dunes
        const float x = wrap(static_cast<float>(i) * 110 - dist * 0.45f, 660) - 90;
        q.ellipse({x, gy + 4}, {70, 18 + static_cast<float>(i % 2) * 8}, p.hill_near, kLayerHills);
    }
    q.rect({-16, gy, 512, dino::kVirtualH - gy + 16}, p.ground, kLayerGround);
    q.rect({-16, gy, 512, 2}, p.ground_line, kLayerGround);
    for (int i = 0; i < 28; ++i) {                              // pebbles scroll with the ground
        const float x = wrap(static_cast<float>(i * 53) - dist, 520) - 20;
        const float y = gy + 5 + static_cast<float>((i * 7) % 40);
        q.rect({x, y, i % 3 == 0 ? 3.0f : 2.0f, 1}, p.speck, kLayerGround);
    }

    const float alpha = ctx.time().alpha;
    for (auto [e, ob] : w.view<dino::Obstacle>())
        draw_obstacle(q, interpolated(w, e, alpha).position, ob, s.play_ticks);
    if (const dino::Runner *r = w.try_get<dino::Runner>(s.runner)) {
        const Vec2 f = interpolated(w, s.runner, alpha).position;
        q.ellipse({f.x + 1, gy + 1}, {10 - std::min(6.0f, (gy - f.y) * 0.08f), 2}, with_alpha(p.ground_line, 0.5f),
                  kLayerGround);
        draw_runner(q, f, *r, s.phase == Phase::Over, dist, p.ink);
    }
}

void label(const Ctx &ctx, std::string_view text, Vec2 pos, float size, Color color, bool centred = false) {
    const Font *font = ctx.res<AssetServer>().get(ctx.res<RenderDefaults>().font);
    if (font == nullptr) return;
    if (centred) pos.x -= measure_text(*font, text, size).x * 0.5f;
    draw_text(ctx.res<RenderQueue>(), *font, text, pos, TextStyle{.size = size, .color = color, .layer = kLayerUi});
}

// Score, title and game-over panels.
void draw_hud(Ctx &ctx) {
    const DinoState &s = ctx.local<DinoState>();
    RenderQueue &q = ctx.res<RenderQueue>();
    const Palette p = mix(night_amount(view_distance(ctx, s) * 0.04f));
    const bool blink = (ctx.time().frame / 20) % 2 == 0;
    char buf[48];
    const auto num = [&buf](const char *pattern, int a, int b) {
        const int len = std::snprintf(buf, sizeof buf, pattern, a, b);
        return std::string_view(buf, static_cast<std::size_t>(std::clamp(len, 0, static_cast<int>(sizeof buf) - 1)));
    };
    const bool milestone = s.phase == Phase::Play && s.score >= 100 && s.score % 100 < 12;
    if (!(milestone && (s.play_ticks / 5) % 2 == 0)) label(ctx, num("%05d", s.score, 0), {418, 10}, 8, p.ink);
    label(ctx, num("HI %05d", std::max(s.best, s.score), 0), {340, 10}, 8, with_alpha(p.ink, 0.65f));

    const Rect panel{90, 70, 300, 96};
    const Color panel_bg = with_alpha(Color{20, 30, 40, 255}, 0.72f);
    if (s.phase == Phase::Title) {
        q.rect(panel, panel_bg, kLayerUi);
        q.rect_lines(panel, 1, kSun, kLayerUi);
        label(ctx, "DINO RUNNER", {240, 84}, 24, kSun, true);
        label(ctx, "SPACE / UP / CLICK  jump (hold = higher)", {240, 118}, 8, kMoon, true);
        label(ctx, "DOWN  duck under birds, fall faster", {240, 130}, 8, kMoon, true);
        if (blink) label(ctx, "PRESS SPACE TO START", {240, 148}, 8, kSun, true);
    } else if (s.phase == Phase::Over) {
        q.rect(panel, panel_bg, kLayerUi);
        q.rect_lines(panel, 1, kBird, kLayerUi);
        label(ctx, "GAME OVER", {240, 84}, 24, kBird, true);
        label(ctx, num("SCORE %d   BEST %d", s.last_score, s.best), {240, 118}, 8, kMoon, true);
        if (s.last_score >= s.best && s.last_score > 0) label(ctx, "NEW HIGH SCORE!", {240, 130}, 8, kSun, true);
        if (s.ticks_in_phase >= kTune.restart_delay && blink)
            label(ctx, "PRESS SPACE TO RUN AGAIN", {240, 148}, 8, kSun, true);
    }
}

EmitterConfig fx(Color a, Color b, float speed_max, float life, float size, float angle, float spread, Vec2 gravity) {
    EmitterConfig c;
    c.capacity = 512;
    c.speed_min = speed_max * 0.3f;
    c.speed_max = speed_max;
    c.angle = angle;
    c.spread = spread;
    c.life_min = life * 0.5f;
    c.life_max = life;
    c.size_start = size;
    c.size_end = 0;
    c.color_start = a;
    c.color_end = b;
    c.gravity = gravity;
    c.drag = 1.2f;
    return c;
}

void startup(Ctx &ctx) {
    World &w = ctx.world();
    dino::setup_world(w);
    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{.position = {dino::kVirtualW / 2, dino::kVirtualH / 2}});
    w.add<CameraShake>(cam, CameraShake{.max_offset = 6});
}

} // namespace

int main(int argc, char **argv) {
    dino::DinoBot bot;                                          // outlives the App
    App app({.title = "Dino Runner", .virtual_size = {480, 270}, .audio = false}, argc, argv);
    app.add_plugins(DefaultPlugins{.render = {.clear_color = kDay.sky_low}});
    app.add_plugin<dino::RulesPlugin>();

    ActionMap &map = app.res<ActionMap>();
    const dino::Controls controls{
        .jump = map.button("jump", {ActionMap::key(Key::Space), ActionMap::key(Key::Up), ActionMap::key(Key::W),
                                    ActionMap::mouse(MouseButton::Left), ActionMap::pad(PadButton::South)}),
        .duck = map.button("duck", {ActionMap::key(Key::Down), ActionMap::key(Key::S), ActionMap::pad(PadButton::East)}),
        .start = map.button("start", {ActionMap::key(Key::Enter), ActionMap::pad(PadButton::Start)})};
    app.insert_resource<dino::Controls>(controls);

    constexpr float kHalfTurn = 3.14159265f;
    AssetServer &assets = app.res<AssetServer>();
    app.insert_resource<dino::Fx>(dino::Fx{
        .dust = assets.add(sanitize(fx({240, 222, 176, 255}, {240, 222, 176, 0}, 70, 0.45f, 2.5f, -kHalfTurn / 2, kHalfTurn * 0.9f,
                                       {0, 140})), "fx/dust"),
        .crash = assets.add(sanitize(fx({231, 111, 81, 255}, {244, 162, 97, 0}, 170, 0.8f, 3.5f, 0, 2 * kHalfTurn, {0, 220})),
                            "fx/crash"),
        .kick = assets.add(sanitize(fx({210, 180, 130, 220}, {210, 180, 130, 0}, 40, 0.3f, 1.5f, -kHalfTurn * 0.8f, kHalfTurn * 0.4f,
                                       {0, 60})), "fx/kick")});

    app.add_startup(startup, "dino.startup");
    app.add_system({.stage = Stage::Render, .fn = draw_world, .name = "dino.draw_world", .run_if = has_local<DinoState>});
    app.add_system({.stage = Stage::RenderUI, .fn = draw_hud, .name = "dino.draw_hud", .run_if = has_local<DinoState>});

    const bool bot_run = app.options().has("--bot");
    if (bot_run && app.options().replay.empty()) {
        bot.controls = controls;
        app.res<InputSystem>().set_driver(&bot);
    }
    const int code = app.run();
    app.res<InputSystem>().set_driver(nullptr);
    if (app.options().headless && bot_run) {
        const World &w = app.main_world();
        const DinoState *s = w.resources().try_get<DinoState>();
        if (s != nullptr)
            std::printf("dino summary: score=%d best=%d runs=%d last=%d speed=%.2f ticks=%" PRIu64 " hash=%016" PRIx64
                        "\n",
                        s->score, std::max(s->best, s->score), s->runs, s->last_score, static_cast<double>(s->speed),
                        app.tick(), w.state_hash());
    }
    return code;
}
