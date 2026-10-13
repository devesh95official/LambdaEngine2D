// Missile Command: drawing, HUD, camera shake and particles. Rules are in src/game/.
//   missile --bot                              watch the bot
//   missile --headless --bot --frames 3000     no window, prints a summary line
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/fixed_string.h>
#include <lam/ecs/world.h>
#include <lam/input/driver.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/render/shapes.h>
#include <game/game.h>

using namespace lam;
using namespace game;

namespace {
constexpr Color kSky = Color::hex(0x0B1026FF), kSky2 = Color::hex(0x141B3AFF);
constexpr Color kGroundC = Color::hex(0x3A2748FF), kGroundTop = Color::hex(0x6B4A7EFF);
constexpr Color kMint = Color::hex(0x7CF2B0FF), kCyan = Color::hex(0x4DD6E8FF);
constexpr Color kAmber = Color::hex(0xFFB547FF), kCoral = Color::hex(0xFF5470FF);
constexpr Color kInk = Color::hex(0xE8ECFFFF), kDim = Color::hex(0x7A82B0FF);

struct Screen {};                                                  // marks the world to draw

void setup(Ctx &ctx) {
    World &w = ctx.world();
    start(w);
    w.resources().insert<Screen>();
    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{.position = {kW / 2, kH / 2}});
    w.add<CameraShake>(cam, CameraShake{.max_offset = 5});
}

void text(RenderQueue &q, const Font &f, std::string_view s, float y, float size, Color c, float x = -1) {
    const Vec2 m = measure_text(f, s, size);
    draw_text(q, f, s, {x < 0 ? std::floor((kW - m.x) / 2) : x, y}, TextStyle{.size = size, .color = c, .shadow = kSky});
}

void draw_world(Ctx &ctx) {
    RenderQueue *qp = ctx.try_res<RenderQueue>();
    if (qp == nullptr) return;
    RenderQueue &q = *qp;
    World &w = ctx.world();
    const Run &r = ctx.local<Run>();
    const std::uint64_t t = w.tick();
    q.rect({-16, -16, kW + 32, kH + 32}, kSky, kLayerBackground);
    q.rect({-16, 150, kW + 32, kGround - 150}, kSky2, kLayerBackground, 1);
    std::uint32_t seed = 12345;                                   // same stars every frame
    for (int i = 0; i < 48; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float x = static_cast<float>(seed % 480u), y = static_cast<float>((seed >> 12) % 200u);
        q.rect({x, y, 1, 1}, (i % 5 == 0 ? kInk : kDim).with_alpha(i % 3 == 0 ? 200 : 110), kLayerBackground, 2);
    }
    q.rect({-16, kGround, kW + 32, kH - kGround + 16}, kGroundC, kLayerTiles);
    q.rect({-16, kGround, kW + 32, 1}, kGroundTop, kLayerTiles, 1);
    for (int b = 0; b < kBases; ++b) {                            // bases: mound + ammo pips
        const float x = kBaseX[static_cast<std::size_t>(b)];
        draw_triangle(q, {x - 24, kGround + 1}, {x + 24, kGround + 1}, {x, kBaseY - 4}, kGroundTop, kLayerTiles, 2);
        const int n = ammo(r, b);
        if (n == 0 && r.phase != kTitle) {
            q.rect({x - 5, kBaseY - 1, 10, 3}, kCoral.with_alpha(160), kLayerWorld);
            continue;
        }
        q.rect({x - 2, kBaseY - 7, 4, 4}, kAmber, kLayerWorld);
        for (int i = 0; i < n; ++i)
            q.rect({x - 13 + static_cast<float>(i % 5) * 6, kGround + 4 + static_cast<float>(i / 5) * 4, 3, 2},
                   kAmber, kLayerWorld);
    }
    for (int i = 0; i < kCities; ++i) {                           // cities: little skylines
        const float x = kCityX[static_cast<std::size_t>(i)];
        if (r.cities >> i & 1u) {
            q.rect({x - 12, kGround - 6, 24, 6}, kCyan.with_alpha(150), kLayerWorld);
            q.rect({x - 9, kGround - 12, 5, 12}, kCyan, kLayerWorld, 1);
            q.rect({x - 2, kGround - 16, 5, 16}, kCyan, kLayerWorld, 1);
            q.rect({x + 5, kGround - 10, 5, 10}, kCyan, kLayerWorld, 1);
            for (int k = 0; k < 3; ++k) q.rect({x - 1 + static_cast<float>(k % 2) * 2, kGround - 14 + static_cast<float>(k) * 4, 1, 1}, kSky, kLayerWorld, 2);
        } else {
            q.rect({x - 11, kGround - 3, 22, 3}, kDim.with_alpha(140), kLayerWorld);
        }
    }
    for (auto [e, m] : w.view<Enemy>()) {
        q.line(m.from, m.pos, 1.5f, kCoral.with_alpha(170), kLayerWorld, 3);
        q.rect({m.pos.x - 1, m.pos.y - 1, 2, 2}, (t / 4) % 2 ? kInk : kCoral, kLayerWorld, 4);
    }
    for (auto [e, c] : w.view<Counter>()) {
        q.line(c.from, c.pos, 1.5f, kMint.with_alpha(170), kLayerWorld, 3);
        q.rect({c.pos.x - 1, c.pos.y - 1, 2, 2}, kInk, kLayerWorld, 4);
        q.line(c.to + Vec2{-3, -3}, c.to + Vec2{3, 3}, 1, kMint, kLayerWorld, 4);
        q.line(c.to + Vec2{-3, 3}, c.to + Vec2{3, -3}, 1, kMint, kLayerWorld, 4);
    }
    constexpr Color kFlash[3] = {kInk, kAmber, kMint};
    for (auto [e, b] : w.view<Blast>()) {
        const float rad = blast_radius(b);
        if (rad <= 0.5f) continue;
        const Color c = b.hostile ? ((t / 3) % 2 ? kCoral : kAmber) : kFlash[(t / 3 + b.age) % 3];
        q.circle(b.pos, rad, c.with_alpha(210), kLayerForeground, 1);
        q.circle(b.pos, rad * 0.55f, kInk.with_alpha(120), kLayerForeground, 2);
    }
    if (r.phase == kPlay) {                                       // crosshair
        const Vec2 p = r.cross;
        q.rect({p.x - 6, p.y, 4, 1}, kInk, kLayerForeground, 5);
        q.rect({p.x + 3, p.y, 4, 1}, kInk, kLayerForeground, 5);
        q.rect({p.x, p.y - 6, 1, 4}, kInk, kLayerForeground, 5);
        q.rect({p.x, p.y + 3, 1, 4}, kInk, kLayerForeground, 5);
    }
}

void draw_hud(Ctx &ctx) {
    RenderQueue *qp = ctx.try_res<RenderQueue>();
    const RenderDefaults *rd = ctx.try_res<RenderDefaults>();
    const Font *f = (qp && rd) ? ctx.res<AssetServer>().get(rd->font) : nullptr;
    if (f == nullptr) return;
    RenderQueue &q = *qp;
    const Run &r = ctx.local<Run>();
    FixedString<64> s;
    if (r.phase != kTitle) {
        format_to(s, "SCORE {}", r.score);
        text(q, *f, s.view(), 4, 8, kInk, 6);
        s.clear();
        format_to(s, "WAVE {}", r.wave);
        text(q, *f, s.view(), 4, 8, kAmber);
    }
    s.clear();
    format_to(s, "HI {}", std::max(r.best, r.score));
    const Vec2 m = measure_text(*f, s.view(), 8);
    text(q, *f, s.view(), 4, 8, kMint, kW - 6 - m.x);
    const bool blink = (ctx.world().tick() / 30) % 2 == 0;
    if (r.phase == kTitle) {
        text(q, *f, "MISSILE COMMAND", 62, 24, kCoral);
        text(q, *f, "DEFEND THE SIX CITIES", 96, 8, kCyan);
        text(q, *f, "MOUSE / WASD / ARROWS AIM", 128, 8, kDim);
        text(q, *f, "CLICK / SPACE FIRES FROM THE NEAREST BASE", 140, 8, kDim);
        text(q, *f, "1 2 3 OR Z X C FIRE FROM A CHOSEN BASE", 152, 8, kDim);
        if (blink) text(q, *f, "PRESS SPACE OR CLICK TO START", 182, 8, kInk);
    } else if (r.phase == kBonus) {
        s.clear();
        format_to(s, "WAVE {} CLEAR", r.wave);
        text(q, *f, s.view(), 90, 16, kMint);
        s.clear();
        format_to(s, "{} CITIES x100 + {} AMMO x5 = {}", city_count(r), r.ammo0 + r.ammo1 + r.ammo2, r.bonus);
        text(q, *f, s.view(), 116, 8, kInk);
    } else if (r.phase == kOver) {
        text(q, *f, "THE END", 84, 32, kCoral);
        s.clear();
        format_to(s, "FINAL SCORE {}   WAVE {}", r.score, r.wave);
        text(q, *f, s.view(), 126, 8, kInk);
        if (r.timer == 0 && blink) text(q, *f, "PRESS SPACE OR CLICK TO PLAY AGAIN", 146, 8, kAmber);
    }
}

void load_fx(App &app) {
    AssetServer &assets = app.res<AssetServer>();
    const EmitterConfig boom{.capacity = 1024, .speed_min = 20, .speed_max = 90, .life_min = 0.25f,
                             .life_max = 0.6f, .size_start = 2, .size_end = 0,
                             .color_start = kAmber, .color_end = kCoral.with_alpha(0), .drag = 2.5f};
    const EmitterConfig debris{.capacity = 1024, .speed_min = 30, .speed_max = 120, .angle = -kTau / 4,
                               .spread = kTau / 2.5f, .life_min = 0.4f, .life_max = 0.9f, .size_start = 2.5f,
                               .size_end = 0.5f, .color_start = kCoral, .color_end = kGroundTop.with_alpha(0),
                               .gravity = {0, 160}, .drag = 1};
    app.insert_resource<Fx>(Fx{.boom = assets.add(sanitize(boom), "fx/boom"),
                               .debris = assets.add(sanitize(debris), "fx/debris")});
}
}

int main(int argc, char **argv) {
    Bot bot;                                                      // outlives the App
    App app({.title = "Missile Command", .virtual_size = {480, 270}}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.add_plugin<RulesPlugin>();
    load_fx(app);
    app.add_startup(setup, "missile.setup");
    app.add_system({.stage = Stage::Render, .fn = draw_world, .name = "missile.draw", .run_if = &has_local<Screen>});
    app.add_system({.stage = Stage::RenderUI, .fn = draw_hud, .name = "missile.hud", .run_if = &has_local<Screen>});
    if (app.options().has("--bot")) app.res<InputSystem>().set_driver(&bot);
    const int code = app.run();
    if (app.options().headless) {
        World &w = app.main_world();
        const Run *rp = w.resources().try_get<Run>();
        const Run r = rp ? *rp : Run{};
        std::printf("missile summary: score=%d best=%d wave=%d cities=%d kills=%u phase=%u ticks=%" PRIu64
                    " hash=%016" PRIx64 "\n",
                    r.score, r.best, r.wave, city_count(r), r.kills, static_cast<unsigned>(r.phase), app.tick(),
                    w.state_hash());
    }
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
