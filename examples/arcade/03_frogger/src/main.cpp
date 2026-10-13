// Frogger: drawing, HUD, camera shake and particles. Rules are in src/frogger/.
//   frogger --bot                             watch the bot
//   frogger --headless --bot --frames 3000    no window, prints a summary line
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
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
#include <lam/render/shapes.h>
#include <frogger/frogger.h>

using namespace lam;
using namespace frogger;

namespace {
constexpr Color kInk = Color::hex(0x0E1222FF), kWater = Color::hex(0x1C3766FF), kWave = Color::hex(0x2B4E8AFF);
constexpr Color kBank = Color::hex(0x1F6B3AFF), kBankDark = Color::hex(0x154A28FF), kVerge = Color::hex(0x4B3B73FF);
constexpr Color kVergeDot = Color::hex(0x5E4C8CFF), kRoad = Color::hex(0x23232EFF), kDash = Color::hex(0x5A5A6EFF);
constexpr Color kFrog = Color::hex(0x7CE36BFF), kFrogDark = Color::hex(0x2F8F3AFF), kWhite_ = Color::hex(0xF4F4F8FF);
constexpr Color kLogC = Color::hex(0x8B5A2BFF), kLogHi = Color::hex(0xB07A45FF), kShell = Color::hex(0xC0392BFF);
constexpr Color kShellHi = Color::hex(0xE8705AFF), kGold = Color::hex(0xFFD54AFF), kRed = Color::hex(0xE84A5FFF);
constexpr Color kPink = Color::hex(0xEB5E8CFF), kGrey = Color::hex(0xC9CAD6FF), kBlue = Color::hex(0x4FA3E8FF);

constexpr std::uint8_t kLBoard = 8, kLMover = 40, kLFrog = 80;

float row_y(float row) { return static_cast<float>(kFieldY) + row * kCell; }

void text(const Ctx &ctx, std::string_view s, Vec2 pos, float size, Color color, bool centred) {
    const RenderDefaults *d = ctx.try_res<RenderDefaults>();
    const Font *font = d ? ctx.res<AssetServer>().get(d->font) : nullptr;
    if (font == nullptr) return;
    if (centred) pos.x -= measure_text(*font, s, size).x * 0.5f;
    draw_text(ctx.res<RenderQueue>(), *font, s, pos,
              TextStyle{.size = size, .color = color, .layer = kLayerUi, .shadow = kInk});
}

void draw_frog(RenderQueue &q, Vec2 c, std::uint8_t facing, float scale, std::uint8_t layer) {
    const float r = 5.5f * scale;
    const Vec2 f = facing == 0 ? Vec2{0, -1} : facing == 1 ? Vec2{1, 0} : facing == 2 ? Vec2{0, 1} : Vec2{-1, 0};
    const Vec2 s{-f.y, f.x};
    for (float k : {-1.0f, 1.0f}) {                             // legs
        q.circle(c + s * (r * 0.95f * k) - f * (r * 0.6f), 2.2f * scale, kFrogDark, layer, 0);
        q.circle(c + s * (r * 0.9f * k) + f * (r * 0.5f), 1.8f * scale, kFrogDark, layer, 0);
    }
    q.circle(c, r, kFrog, layer, 1);
    q.circle(c - f * 1.5f, r * 0.45f, kFrogDark.with_alpha(110), layer, 2);
    for (float k : {-1.0f, 1.0f}) {                             // eyes
        const Vec2 e = c + f * (r * 0.55f) + s * (2.4f * k * scale);
        q.circle(e, 1.8f * scale, kWhite_, layer, 3);
        q.circle(e + f * 0.6f, 0.9f * scale, kInk, layer, 4);
    }
}

void draw_mover(RenderQueue &q, const Mover &m, float x, std::uint32_t clock) {
    const float y = row_y(static_cast<float>(m.row));
    const float w = static_cast<float>(m.w);
    const bool right = m.base_speed > 0;
    switch (m.kind) {
    case kLog:
        q.rect({x, y + 3, w, 10}, kLogC, kLMover, 0);
        q.rect({x + 2, y + 4, w - 4, 2}, kLogHi, kLMover, 1);
        q.rect({x, y + 3, 3, 10}, kLogHi, kLMover, 1);
        for (float k = 10; k < w - 6; k += 14) q.rect({x + k, y + 8, 5, 1}, kLogHi.with_alpha(120), kLMover, 1);
        break;
    case kTurtle: {
        const int ph = turtle_phase(m, clock);
        const std::uint8_t a = ph == 0 ? 255 : ph == 1 ? 140 : 40;
        for (float k = 0; k < w; k += 16) {
            const Vec2 c{x + k + 8, y + 8};
            q.circle(c + Vec2{right ? 6.0f : -6.0f, 0}, 2.0f, kFrog.with_alpha(a), kLMover, 0);
            q.circle(c, 6, kShell.with_alpha(a), kLMover, 1);
            q.circle(c - Vec2{1, 1}, 3, kShellHi.with_alpha(a), kLMover, 2);
        }
        break;
    }
    case kTruck:
        q.rect({x, y + 3, w, 10}, kGrey, kLMover, 0);
        q.rect({right ? x + w - 8 : x, y + 2, 8, 12}, kBlue, kLMover, 1);
        q.rect({right ? x + w - 4 : x + 1, y + 4, 3, 8}, kWater, kLMover, 2);
        break;
    case kRacer:
        q.rect({x, y + 5, w, 6}, kWhite_, kLMover, 0);
        q.rect({right ? x + w - 6 : x + 2, y + 6, 4, 4}, kRed, kLMover, 1);
        break;
    case kDozer:
        q.rect({x + 1, y + 3, w - 2, 10}, kFrogDark, kLMover, 0);
        q.rect({right ? x + w - 2 : x - 1, y + 2, 3, 12}, kGold, kLMover, 1);
        break;
    default: {
        const Color body = m.row == 9 ? kPink : kGold;
        q.rect({x + 1, y + 2, 3, 12}, kInk, kLMover, 0);
        q.rect({x + w - 4, y + 2, 3, 12}, kInk, kLMover, 0);
        q.rect({x, y + 4, w, 8}, body, kLMover, 1);
        q.rect({right ? x + w - 6 : x + 2, y + 5, 4, 6}, kWater, kLMover, 2);
        break;
    }
    }
}

// Board on world layers so the camera shake moves it.
void draw_board(Ctx &ctx) {
    RenderQueue &q = ctx.res<RenderQueue>();
    World &w = ctx.world();
    const Run &run = ctx.local<Run>();
    const float alpha = ctx.time().alpha;
    const float W = static_cast<float>(kW);
    q.rect({-40, -40, W + 80, kVH + 80.0f}, kInk, kLBoard, 0);
    q.rect({0, row_y(0), W, kCell}, kBank, kLBoard, 1);
    for (int k = 0; k < kHomes; ++k) {
        const float sx = static_cast<float>(slot_cx(k));
        q.rect({sx - 11, row_y(0) + 3, 22, 13}, kWater, kLBoard, 2);
        q.rect({sx - 11, row_y(0) + 3, 22, 2}, kBankDark, kLBoard, 3);
        if (run.homes & (1u << k)) draw_frog(q, {sx, row_y(0) + 9}, 2, 0.9f, kLFrog);
    }
    q.rect({0, row_y(1), W, 5 * kCell}, kWater, kLBoard, 1);
    const float drift = static_cast<float>(run.clock % 64) * 0.5f;
    for (int r = 1; r <= 5; ++r)
        for (float x = -32 + std::fmod(drift * (r % 2 ? 1.0f : -1.0f) + 64, 32.0f); x < W; x += 32)
            q.rect({x, row_y(static_cast<float>(r)) + static_cast<float>(4 + (r * 5) % 8), 8, 1}, kWave, kLBoard, 2);
    for (int r : {6, 12}) {
        q.rect({0, row_y(static_cast<float>(r)), W, kCell}, kVerge, kLBoard, 1);
        for (float x = 4; x < W; x += 12) q.rect({x, row_y(static_cast<float>(r)) + 7, 2, 2}, kVergeDot, kLBoard, 2);
    }
    q.rect({0, row_y(7), W, 5 * kCell}, kRoad, kLBoard, 1);
    for (int r = 8; r <= 11; ++r)
        for (float x = 2; x < W; x += 16) q.rect({x, row_y(static_cast<float>(r)), 8, 1}, kDash, kLBoard, 2);

    const bool moving = true;
    for (auto [e, m] : w.view<Mover>()) {
        const float x = (static_cast<float>(m.x) - (moving ? static_cast<float>(speed_of(m, run.level)) * (1 - alpha) : 0)) / kMilli;
        draw_mover(q, m, x, run.clock);
    }

    if (run.state == kPlay || run.state == kDying) {
        float p = 1;
        if (run.hop > 0) p = std::clamp((static_cast<float>(kHopTicks - run.hop) + alpha) / kHopTicks, 0.0f, 1.0f);
        const Vec2 from{static_cast<float>(run.from_cx) / kMilli, row_y(static_cast<float>(run.from_row)) + 8};
        const Vec2 to{static_cast<float>(run.cx) / kMilli, row_y(static_cast<float>(run.row)) + 8};
        const Vec2 c = from + (to - from) * p;
        if (run.state == kPlay) {
            draw_frog(q, c, run.facing, 1.0f + 0.25f * std::sin(p * 3.14159f), kLFrog);
        } else {
            const float t = static_cast<float>(run.state_timer) / 45.0f;
            if (run.death == 2) {
                q.circle(c, 3 + 8 * t, kWave.with_alpha(static_cast<std::uint8_t>(200 * (1 - t))), kLFrog, 0);
                q.circle(c, 2 + 5 * t, kWater, kLFrog, 1);
            } else {
                q.ellipse(c, {8, 4}, kFrogDark, kLFrog, 0);
                q.line(c - Vec2{4, 4}, c + Vec2{4, 4}, 2, kRed, kLFrog, 1);
                q.line(c - Vec2{4, -4}, c + Vec2{4, -4}, 2, kRed, kLFrog, 1);
            }
        }
    }
}

// HUD and the title / game-over cards, in screen space.
void draw_hud(Ctx &ctx) {
    RenderQueue &q = ctx.res<RenderQueue>();
    const Run &run = ctx.local<Run>();
    const float W = static_cast<float>(kW);
    q.rect({0, 0, W, kFieldY}, kInk, kLayerUi);
    q.rect({0, row_y(kRows), W, kVH - row_y(kRows)}, kInk, kLayerUi);
    FixedString<32> s;
    format_to(s, "SCORE {}", run.score);
    text(ctx, s.view(), {3, 4}, 8, kWhite_, false);
    s.clear();
    format_to(s, "HI {}", run.best);
    text(ctx, s.view(), {W - 3 - static_cast<float>(s.view().size()) * 6, 4}, 8, kGold, false);
    s.clear();
    format_to(s, "L{}", run.level);
    text(ctx, s.view(), {W / 2, 4}, 8, kVergeDot.with_alpha(255), true);
    const float by = row_y(kRows) + 4;
    for (int i = 0; i < std::max(0, run.lives - (run.state == kPlay ? 1 : 0)); ++i)
        draw_frog(q, {8.0f + static_cast<float>(i) * 12, by + 4}, 0, 0.7f, kLayerUi);
    if (run.state == kPlay || run.state == kDying) {
        const float frac = static_cast<float>(run.timer) / kFrogTicks;
        const float bw = 90;
        q.rect({W - bw - 30, by + 2, bw, 5}, kRoad, kLayerUi);
        q.rect({W - 30 - bw * frac, by + 2, bw * frac, 5}, frac < 0.25f ? kRed : kFrog, kLayerUi);
        text(ctx, "TIME", {W - 27, by}, 8, kGold, false);
    }
    const bool blink = (run.clock / 30) % 2 == 0;
    if (run.state == kTitle) {
        q.rect({8, 56, W - 16, 128}, kInk.with_alpha(225), kLayerUi);
        q.rect_lines({8, 56, W - 16, 128}, 1, kFrog, kLayerUi);
        text(ctx, "FROGGER", {W / 2, 68}, 24, kFrog, true);
        text(ctx, "ARROWS / WASD: HOP", {W / 2, 104}, 8, kWhite_, true);
        text(ctx, "CLICK: HOP TOWARD CURSOR", {W / 2, 116}, 8, kWhite_, true);
        text(ctx, "FILL ALL 5 HOMES", {W / 2, 132}, 8, kGold, true);
        if (blink) text(ctx, "PRESS SPACE", {W / 2, 160}, 8, kGold, true);
    } else if (run.state == kOver) {
        q.rect({16, 72, W - 32, 96}, kInk.with_alpha(225), kLayerUi);
        q.rect_lines({16, 72, W - 32, 96}, 1, kRed, kLayerUi);
        text(ctx, "GAME OVER", {W / 2, 82}, 16, kRed, true);
        s.clear();
        format_to(s, "SCORE {}", run.score);
        text(ctx, s.view(), {W / 2, 108}, 8, kWhite_, true);
        s.clear();
        format_to(s, "HI {}", run.best);
        text(ctx, s.view(), {W / 2, 122}, 8, kGold, true);
        if (run.state_timer > 40 && blink) text(ctx, "SPACE TO RESTART", {W / 2, 146}, 8, kGold, true);
    }
}

void setup(Ctx &ctx) {
    World &w = ctx.world();
    start_world(w);
    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{.position = {kW / 2.0f, kVH / 2.0f}});
    w.add<CameraShake>(cam, CameraShake{.max_offset = 4});
}

void load_fx(App &app) {
    AssetServer &assets = app.res<AssetServer>();
    const EmitterConfig splash{.capacity = 256, .speed_min = 20, .speed_max = 70, .life_min = 0.3f,
                               .life_max = 0.6f, .size_start = 2.5f, .color_start = Color::hex(0xBFE3FFFF),
                               .color_end = Color::hex(0x4FA3E800), .gravity = {0, 120}, .drag = 1};
    const EmitterConfig squash{.capacity = 256, .speed_min = 30, .speed_max = 90, .life_min = 0.25f,
                               .life_max = 0.5f, .size_start = 2.5f, .color_start = Color::hex(0x7CE36BFF),
                               .color_end = Color::hex(0xE84A5F00), .drag = 3};
    const EmitterConfig sparkle{.capacity = 512, .speed_min = 15, .speed_max = 60, .life_min = 0.4f,
                                .life_max = 0.8f, .size_start = 2, .color_start = Color::hex(0xFFF2A0FF),
                                .color_end = Color::hex(0xFFD54A00), .gravity = {0, -30}, .drag = 1.5f};
    app.insert_resource<Fx>(Fx{.splash = assets.add(sanitize(splash), "fx/splash"),
                               .squash = assets.add(sanitize(squash), "fx/squash"),
                               .sparkle = assets.add(sanitize(sparkle), "fx/sparkle")});
}
} // namespace

int main(int argc, char **argv) {
    Bot bot;                                                    // outlives the App
    App app({.title = "Frogger", .window_size = {624, 720}, .virtual_size = {kW, kVH}}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.add_plugin<RulesPlugin>();
    load_fx(app);
    app.add_startup(setup, "frogger.setup");
    app.add_system({.stage = Stage::Render, .fn = draw_board, .name = "frogger.board", .run_if = &has_local<Run>});
    app.add_system({.stage = Stage::RenderUI, .fn = draw_hud, .name = "frogger.hud", .run_if = &has_local<Run>});
    if (app.options().has("--bot")) app.res<InputSystem>().set_driver(&bot);
    const int code = app.run();
    if (app.options().headless) {
        World &w = app.main_world();
        const Run *r = w.resources().try_get<Run>();
        const Run run = r ? *r : Run{};
        std::printf("frogger summary: score=%d best=%d level=%d lives=%d homed=%u deaths=%u hops=%u ticks=%" PRIu64
                    " hash=%016" PRIx64 "\n",
                    run.score, run.best, run.level, run.lives, run.homed, run.deaths, run.hops, app.tick(),
                    w.state_hash());
    }
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
