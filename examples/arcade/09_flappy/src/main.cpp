// Flappy: drawing, camera shake and particles. Rules are in src/flappy/.
//   flappy --bot                                 watch the bot
//   flappy --headless --bot --frames 3000        no window, prints a summary line
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/fixed_string.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/color.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include <flappy/flappy.h>

using namespace lam;
using namespace flappy;

namespace {
constexpr Color kSkyTop = Color::hex(0x5BB8E0FF), kSkyLow = Color::hex(0xBDE8F2FF);
constexpr Color kCloud = Color::hex(0xF6FBFDFF), kHillFar = Color::hex(0xA6DCC0FF), kHillNear = Color::hex(0x7CC79AFF);
constexpr Color kPipe = Color::hex(0x5DBB63FF), kPipeDark = Color::hex(0x2F6B3AFF), kPipeLight = Color::hex(0xA3E37FFF);
constexpr Color kSand = Color::hex(0xE6C982FF), kSandDark = Color::hex(0xD3AE63FF), kGrass = Color::hex(0x76C04BFF);
constexpr Color kBirdBody = Color::hex(0xFFCF3FFF), kBirdWing = Color::hex(0xF2A41FFF), kBeak = Color::hex(0xF2652EFF);
constexpr Color kInk = Color::hex(0x2B2D42FF), kWhiteC = Color::hex(0xFFFFFFFF);
constexpr std::uint8_t kLSky = 0, kLCloud = 4, kLHill = 8, kLPipe = 16, kLGround = 64, kLBird = 100;

Color lerp_color(Color a, Color b, float t) {
    const auto m = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t);
    };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}
Vec2 rot(Vec2 v, float a) { return {v.x * std::cos(a) - v.y * std::sin(a), v.x * std::sin(a) + v.y * std::cos(a)}; }
float wrap(float x, float period) { return x - period * std::floor(x / period); }
std::uint32_t hash(std::int64_t i) {
    auto x = static_cast<std::uint32_t>(i * 2654435761LL);
    x ^= x >> 15;
    return x * 2246822519u;
}

void start(Ctx &ctx) {
    start_game(ctx.world());
    const Entity cam = ctx.world().create();                    // only here for the shake
    ctx.world().add<Camera2D>(cam, Camera2D{.position = {kW * 0.5f, kH * 0.5f}});
    ctx.world().add<CameraShake>(cam, CameraShake{.max_offset = 6});
}

void draw_world(Ctx &ctx) {
    RenderQueue *qp = ctx.try_res<RenderQueue>();
    if (qp == nullptr) return;
    RenderQueue &q = *qp;
    World &w = ctx.world();
    const Run &run = ctx.local<Run>();
    const float alpha = ctx.time().alpha;
    const float sx = (static_cast<float>(run.scroll) - (run.phase == kOver ? 0.0f : 1.0f - alpha)) * kScroll;
    // Sky bands.
    constexpr int kBands = 16;
    for (int i = 0; i < kBands; ++i) {
        const float y0 = kGroundY * static_cast<float>(i) / kBands;
        q.rect(Rect{-16, y0 - 16 * (i == 0), kW + 32, kGroundY / kBands + 17}, lerp_color(kSkyTop, kSkyLow, static_cast<float>(i) / (kBands - 1)), kLSky);
    }
    // Parallax: clouds 0.2x, hills 0.4x, bushes 0.7x.
    const auto layer = [&](float factor, float period, auto &&draw) {
        const float off = sx * factor;
        const auto first = static_cast<std::int64_t>(std::floor(off / period)) - 1;
        for (std::int64_t k = first; k < first + static_cast<std::int64_t>(kW / period) + 3; ++k)
            draw(static_cast<float>(k) * period - off, hash(k));
    };
    layer(0.2f, 96, [&](float x, std::uint32_t h) {
        const float y = 36 + static_cast<float>(h % 70), s = 0.8f + static_cast<float>((h >> 8) % 5) * 0.12f;
        q.circle({x, y}, 11 * s, kCloud, kLCloud);
        q.circle({x + 12 * s, y - 5 * s}, 13 * s, kCloud, kLCloud);
        q.circle({x + 26 * s, y}, 10 * s, kCloud, kLCloud);
        q.rect(Rect{x, y, 26 * s, 10 * s}, kCloud, kLCloud);
    });
    layer(0.4f, 70, [&](float x, std::uint32_t h) {
        q.circle({x, kGroundY + 14}, 38 + static_cast<float>(h % 18), kHillFar, kLHill);
    });
    layer(0.7f, 38, [&](float x, std::uint32_t h) {
        q.circle({x, kGroundY + 4}, 14 + static_cast<float>(h % 8), kHillNear, kLHill, 1);
    });
    // Pipes.
    for (auto [e, t, p] : w.view<Transform, Pipe>()) {
        const float x = interpolated(w, e, alpha).position.x - kPipeW * 0.5f;
        const float top = p.gap_y - kGap * 0.5f, bot = p.gap_y + kGap * 0.5f;
        for (const Rect body : {Rect{x, -20, kPipeW, top + 20}, Rect{x, bot, kPipeW, kGroundY - bot}}) {
            q.rect(body, kPipe, kLPipe);
            q.rect(Rect{body.x + 5, body.y, 6, body.h}, kPipeLight, kLPipe);
            q.rect_lines(body, 2, kPipeDark, kLPipe);
        }
        for (const Rect lip : {Rect{x - 4, top - 14, kPipeW + 8, 14}, Rect{x - 4, bot, kPipeW + 8, 14}}) {
            q.rect(lip, kPipe, kLPipe);
            q.rect(Rect{lip.x + 6, lip.y + 2, 6, lip.h - 4}, kPipeLight, kLPipe);
            q.rect_lines(lip, 2, kPipeDark, kLPipe);
        }
    }
    // Ground scrolls at 1x.
    q.rect(Rect{-16, kGroundY, kW + 32, kH - kGroundY + 16}, kSand, kLGround);
    for (float x = -wrap(sx, 16) - 16; x < kW + 16; x += 16)
        q.triangle({x, kGroundY + 8}, {x + 8, kGroundY + 8}, {x + 2, kGroundY + 20}, kSandDark, kLGround),
        q.triangle({x + 8, kGroundY + 8}, {x + 10, kGroundY + 20}, {x + 2, kGroundY + 20}, kSandDark, kLGround);
    q.rect(Rect{-16, kGroundY, kW + 32, 7}, kGrass, kLGround);
    q.rect(Rect{-16, kGroundY + 7, kW + 32, 2}, kPipeDark, kLGround);
    // Bird tilts with fall speed and bobs on the title screen.
    const Entity b = find_bird(w);
    if (!b) return;
    Vec2 c = interpolated(w, b, alpha).position;
    const float vy = w.get<Bird>(b).vy;
    const auto secs = static_cast<float>(ctx.time().real_seconds);
    if (run.phase == kTitle) c.y += std::sin(secs * 4.0f) * 5.0f;
    const float tilt = run.phase == kTitle ? 0.0f : run.phase == kOver ? 1.3f : std::clamp(vy * 0.14f, -0.45f, 1.3f);
    const float wing = run.phase == kOver ? 0.0f : std::sin(secs * 18.0f) * 3.0f;
    const auto at = [&](float x, float y) { return c + rot({x, y}, tilt); };
    q.circle(c, 8, kInk, kLBird);
    q.circle(c, 7, kBirdBody, kLBird);
    q.circle(at(1, 3), 4, kWhiteC.with_alpha(110), kLBird);    // belly
    q.ellipse(at(-4, wing * 0.5f), {5, 3}, kBirdWing, kLBird);
    q.circle(at(3, -3), 3, kWhiteC, kLBird);
    q.circle(at(4, -3), 1.5f, kInk, kLBird);
    q.triangle(at(6, -1), at(12, 1), at(6, 3), kBeak, kLBird);
}

void text_centered(RenderQueue &q, const Font &f, std::string_view s, float y, float size, Color c) {
    const Vec2 m = measure_text(f, s, size);
    const Vec2 at{std::floor((kW - m.x) * 0.5f), y};
    const float d = std::max(1.0f, std::floor(size / 10.0f));  // drop shadow
    draw_text(q, f, s, at + Vec2{d, d}, TextStyle{.size = size, .color = kInk});
    draw_text(q, f, s, at, TextStyle{.size = size, .color = c});
}

void draw_hud(Ctx &ctx) {
    RenderQueue *qp = ctx.try_res<RenderQueue>();
    const Font *font = ctx.res<AssetServer>().get(ctx.res<RenderDefaults>().font);
    if (qp == nullptr || font == nullptr) return;
    RenderQueue &q = *qp;
    const Run &run = ctx.local<Run>();
    const bool blink = std::fmod(ctx.time().real_seconds, 1.0) < 0.6;
    FixedString<64> s;
    if (run.phase == kTitle) {
        text_centered(q, *font, "FLAPPY", 60, 32, kBirdBody);
        text_centered(q, *font, "SPACE / CLICK TO FLAP", 104, 8, kWhiteC);
        if (blink) text_centered(q, *font, "FLAP TO START", 216, 8, kWhiteC);
        if (run.best > 0) {
            format_to(s, "BEST {}", run.best);
            text_centered(q, *font, s.view(), 232, 8, kWhiteC);
        }
        return;
    }
    format_to(s, "{}", run.score);
    text_centered(q, *font, s.view(), 18, 24, kWhiteC);
    if (run.phase != kOver) return;
    const Rect panel{40, 96, kW - 80, 92};
    q.rect(Rect{panel.x + 3, panel.y + 3, panel.w, panel.h}, kInk.with_alpha(120), kLayerUi);
    q.rect(panel, kSand, kLayerUi);
    q.rect_lines(panel, 2, kInk, kLayerUi);
    text_centered(q, *font, "GAME OVER", panel.y + 10, 16, kBeak);
    s.clear();
    format_to(s, "SCORE {}   BEST {}", run.score, run.best);
    text_centered(q, *font, s.view(), panel.y + 38, 8, kWhiteC);
    if (run.score > 0 && run.score >= run.best) text_centered(q, *font, "NEW BEST!", panel.y + 54, 8, kBirdBody);
    if (run.phase_ticks >= kRestartDelay && blink) text_centered(q, *font, "FLAP TO RESTART", panel.y + 72, 8, kWhiteC);
}

void load_fx(App &app) {
    AssetServer &a = app.res<AssetServer>();
    const EmitterConfig feathers{.capacity = 256, .speed_min = 50, .speed_max = 150, .life_min = 0.5f, .life_max = 1.1f,
                                 .size_start = 3.5f, .size_end = 1, .color_start = kBirdBody,
                                 .color_end = kBirdWing.with_alpha(0), .gravity = {0, 260}, .drag = 1.5f};
    const EmitterConfig sparkle{.capacity = 256, .speed_min = 20, .speed_max = 80, .life_min = 0.25f, .life_max = 0.55f,
                                .size_start = 2.5f, .size_end = 0, .color_start = kWhiteC,
                                .color_end = Color::hex(0xFFE27A00), .drag = 2};
    const EmitterConfig puff{.capacity = 128, .speed_min = 15, .speed_max = 40, .angle = 2.6f, .spread = 1.2f,
                             .life_min = 0.2f, .life_max = 0.35f, .size_start = 3, .size_end = 0,
                             .color_start = kWhiteC.with_alpha(210), .color_end = kWhiteC.with_alpha(0)};
    app.insert_resource<Fx>(Fx{.feathers = a.add(sanitize(feathers), "fx/feathers"),
                               .sparkle = a.add(sanitize(sparkle), "fx/sparkle"),
                               .puff = a.add(sanitize(puff), "fx/puff")});
}
}

int main(int argc, char **argv) {
    Bot bot;                                                    // outlives the App
    App app({.title = "Flappy", .window_size = {480, 720}, .virtual_size = {240, 360}}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.add_plugin<RulesPlugin>();
    load_fx(app);
    app.add_startup(start, "flappy.start");
    app.add_system({.stage = Stage::Render, .fn = draw_world, .name = "flappy.draw", .run_if = &has_local<Run>});
    app.add_system({.stage = Stage::RenderUI, .fn = draw_hud, .name = "flappy.hud", .run_if = &has_local<Run>});
    if (app.options().has("--bot")) {
        bot.controls = app.res<Controls>();
        app.res<InputSystem>().set_driver(&bot);
    }
    const int code = app.run();
    if (app.options().headless) {
        World &w = app.main_world();
        const Run *r = w.resources().try_get<Run>();
        const Run run = r ? *r : Run{};
        std::printf("flappy summary: score=%d best=%d games=%d deaths=%d flaps=%d phase=%d pipes=%zu ticks=%llu "
                    "hash=%016llx\n",
                    run.score, run.best, run.games, run.deaths, run.flaps, run.phase, w.count<Pipe>(),
                    static_cast<unsigned long long>(app.tick()), static_cast<unsigned long long>(w.state_hash()));
    }
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
