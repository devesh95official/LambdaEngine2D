// Asteroids: drawing, HUD, particles and camera shake. Rules are in src/asteroids/.
//   asteroids --bot                            watch the bot
//   asteroids --headless --bot --frames 3000   no window, prints a summary line
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/fixed_string.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/scalar.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/font.h>
#include <lam/render/render_plugin.h>
#include <lam/render/render_queue.h>
#include <lam/render/shapes.h>
#include <lam/render/sprite.h>
#include <asteroids/game.h>

using namespace lam;

namespace {
constexpr Color kBg = Color::hex(0x0A0D1CFF);
constexpr Color kStar = Color::hex(0xC8D3FFFF);
constexpr Color kShip = Color::hex(0x6EF3FFFF);
constexpr Color kShipFill = Color::hex(0x123048FF);
constexpr Color kRock = Color::hex(0xF4C26BFF);
constexpr Color kRockFill = Color::hex(0x2B2340FF);
constexpr Color kBullet = Color::hex(0xFFF6C8FF);
constexpr Color kFlame = Color::hex(0xFFA53DFF);
constexpr Color kAccent = Color::hex(0xFF5C8AFF);
constexpr Color kText = Color::hex(0xE8ECFFFF);
constexpr std::uint8_t kLayerRocks = 40, kLayerShots = 50, kLayerShip = 60;

std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
float hash01(std::uint32_t x) { return static_cast<float>(hash32(x) >> 8) * (1.0f / 16777216.0f); }

void setup(Ctx &ctx) {
    World &w = ctx.world();
    ast::start_world(w);
    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{.position = {ast::kW / 2, ast::kH / 2}});
    w.add<CameraShake>(cam, CameraShake{.max_offset = 7});
}

// Calls fn(offset) for each wrapped copy of a circle that is on screen.
template <class F> void each_wrap(Vec2 p, float r, F &&fn) {
    for (float ox : {-ast::kW, 0.0f, ast::kW})
        for (float oy : {-ast::kH, 0.0f, ast::kH}) {
            const Vec2 q = p + Vec2{ox, oy};
            if (q.x + r < 0 || q.x - r > ast::kW || q.y + r < 0 || q.y - r > ast::kH) continue;
            fn(Vec2{ox, oy});
        }
}

void polygon(RenderQueue &q, Vec2 c, std::span<const Vec2> pts, Color fill, Color line, std::uint8_t layer) {
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Vec2 a = c + pts[i], b = c + pts[(i + 1) % pts.size()];
        draw_triangle(q, c, a, b, fill, layer, 0);
    }
    for (std::size_t i = 0; i < pts.size(); ++i)
        q.line(c + pts[i], c + pts[(i + 1) % pts.size()], 1.5f, line, layer, 1);
}

void draw_world(Ctx &ctx) {
    RenderQueue *qp = ctx.try_res<RenderQueue>();
    if (qp == nullptr) return;
    RenderQueue &q = *qp;
    World &w = ctx.world();
    const float alpha = ctx.time().alpha;
    for (std::uint32_t i = 0; i < 110; ++i) {                    // fixed starfield
        const Vec2 p{hash01(i * 3 + 1) * ast::kW, hash01(i * 3 + 2) * ast::kH};
        const float b = hash01(i * 3 + 3);
        const float s = b > 0.9f ? 2.0f : 1.0f;
        q.rect(Rect{p.x, p.y, s, s}, kStar.with_alpha(static_cast<std::uint8_t>(50 + b * 150)), kLayerBackground);
    }
    for (auto [e, rock, body] : w.view<ast::Rock, ast::Body>()) {
        const Transform t = interpolated(w, e, alpha);
        constexpr std::size_t kN = 11;
        std::array<Vec2, kN> pts{};
        for (std::size_t i = 0; i < kN; ++i) {
            const float a = t.rotation + kTau * static_cast<float>(i) / kN;
            const float r = body.radius * (0.62f + 0.45f * hash01(rock.shape + static_cast<std::uint32_t>(i) * 7919u));
            pts[i] = from_angle(a, r);
        }
        each_wrap(t.position, body.radius, [&](Vec2 o) { polygon(q, t.position + o, pts, kRockFill, kRock, kLayerRocks); });
    }
    for (auto [e, body] : w.view<ast::Body>(With<ast::Bullet>{})) {
        const Vec2 p = interpolated(w, e, alpha).position;
        q.line(p - normalize_or(body.vel) * 5.0f, p, 1.0f, kFlame.with_alpha(150), kLayerShots, 0);
        q.circle(p, 1.8f, kBullet, kLayerShots, 1);
    }
    for (auto [e, ship, body] : w.view<ast::Ship, ast::Body>()) {
        if (ship.invuln > 0 && (ship.invuln / 6) % 2 == 1) continue;   // respawn blink
        const Transform t = interpolated(w, e, alpha);
        const auto rot = [&](float x, float y) { return rotate(Vec2{x, y}, t.rotation); };
        const std::array<Vec2, 4> hull{rot(12, 0), rot(-8, -7), rot(-4, 0), rot(-8, 7)};
        each_wrap(t.position, 14, [&](Vec2 o) {
            const Vec2 c = t.position + o;
            if (ship.thrusting) {
                const float len = 9.0f + ctx.fx_rng().unit() * 6.0f;
                draw_triangle(q, c + rot(-5, -3.5f), c + rot(-5, 3.5f), c + rot(-5 - len, 0), kFlame, kLayerShip, 0);
            }
            polygon(q, c, hull, kShipFill, kShip, kLayerShip);
        });
    }
}

void label(const Ctx &ctx, std::string_view text, Vec2 pos, float size, Color color, bool centred = true) {
    const RenderDefaults *defaults = ctx.try_res<RenderDefaults>();
    const Font *font = defaults ? ctx.res<AssetServer>().get(defaults->font) : nullptr;
    if (font == nullptr) return;
    if (centred) pos.x -= std::floor(measure_text(*font, text, size).x * 0.5f);
    draw_text(ctx.res<RenderQueue>(), *font, text, pos,
              TextStyle{.size = size, .color = color, .layer = kLayerUi, .shadow = Color{0, 0, 0, 200}});
}

void draw_hud(Ctx &ctx) {
    RenderQueue *qp = ctx.try_res<RenderQueue>();
    if (qp == nullptr) return;
    RenderQueue &q = *qp;
    const ast::Run &run = ctx.local<ast::Run>();
    const bool blink = (ctx.time().tick / 30) % 2 == 0;
    FixedString<64> s;
    if (run.phase == ast::kTitle) {
        q.rect(Rect{0, 0, ast::kW, ast::kH}, kBg.with_alpha(110), kLayerUi);
        label(ctx, "ASTEROIDS", {ast::kW / 2, 92}, 48, kAccent);
        label(ctx, "a LambdaEngine2D mini game", {ast::kW / 2, 150}, 8, kRock);
        if (blink) label(ctx, "PRESS ENTER OR SPACE TO START", {ast::kW / 2, 200}, 16, kText);
        label(ctx, "LEFT/RIGHT or A/D turn   UP/W or right-click thrust", {ast::kW / 2, 250}, 8, kShip);
        label(ctx, "SPACE or left-click fire   (hold for auto-fire)", {ast::kW / 2, 264}, 8, kShip);
        format_to(s, "HIGH SCORE {}", run.best);
        label(ctx, s.view(), {ast::kW / 2, 300}, 16, kRock);
        return;
    }
    format_to(s, "{:06}", run.score);
    label(ctx, s.view(), {10, 8}, 16, kText, false);
    s.clear();
    format_to(s, "HI {:06}", run.best);
    label(ctx, s.view(), {ast::kW / 2, 8}, 8, kRock);
    s.clear();
    format_to(s, "WAVE {}", std::max(run.wave, 1));
    label(ctx, s.view(), {ast::kW / 2, 20}, 8, kShip);
    for (int i = 0; i < std::min(run.lives, 8); ++i) {         // spare ships, top right
        const Vec2 c{ast::kW - 14.0f - static_cast<float>(i) * 16.0f, 18};
        const std::array<Vec2, 4> icon{Vec2{0, -8}, Vec2{-6, 6}, Vec2{0, 3}, Vec2{6, 6}};
        polygon(q, c, icon, kShipFill, kShip, kLayerUi);
    }
    if (run.phase == ast::kPlay) {
        if (run.wave_delay > 0 && run.wave_delay < 100) {
            s.clear();
            format_to(s, "WAVE {}", run.wave + 1);
            label(ctx, s.view(), {ast::kW / 2, 110}, 24, kAccent);
        }
        if (run.respawn > 0) label(ctx, "GET READY", {ast::kW / 2, 230}, 16, kText);
        return;
    }
    q.rect(Rect{0, 0, ast::kW, ast::kH}, kBg.with_alpha(120), kLayerUi);
    label(ctx, "GAME OVER", {ast::kW / 2, 110}, 40, kAccent);
    s.clear();
    format_to(s, "SCORE {}   BEST {}", run.score, run.best);
    label(ctx, s.view(), {ast::kW / 2, 170}, 16, kText);
    if (run.over_ticks > 45 && blink) label(ctx, "PRESS ENTER TO PLAY AGAIN", {ast::kW / 2, 220}, 16, kRock);
}

void load_fx(App &app) {
    AssetServer &assets = app.res<AssetServer>();
    const EmitterConfig boom{.capacity = 1024, .speed_min = 30, .speed_max = 170, .life_min = 0.3f,
                             .life_max = 1.0f, .size_start = 3, .color_start = Color::hex(0xFFE08AFF),
                             .color_end = Color::hex(0xFF3D6E00), .drag = 1.2f};
    const EmitterConfig spark{.capacity = 1024, .speed_min = 20, .speed_max = 110, .life_min = 0.25f,
                              .life_max = 0.7f, .size_start = 2.5f, .color_start = kRock,
                              .color_end = Color::hex(0x8A5CFF00), .drag = 1.5f};
    const EmitterConfig exhaust{.capacity = 256, .speed_min = 5, .speed_max = 30, .life_min = 0.12f,
                                .life_max = 0.3f, .size_start = 2, .color_start = kFlame,
                                .color_end = Color::hex(0xFF3D0000)};
    app.insert_resource<ast::Fx>(ast::Fx{.boom = assets.add(sanitize(boom), "fx/boom"),
                                         .spark = assets.add(sanitize(spark), "fx/spark"),
                                         .exhaust = assets.add(sanitize(exhaust), "fx/exhaust")});
}
}

int main(int argc, char **argv) {
    ast::Bot bot;                                               // outlives the App
    App app({.title = "Asteroids", .window_size = {1280, 720}, .virtual_size = {640, 360}}, argc, argv);
    app.add_plugins(DefaultPlugins{.render = RenderConfig{.clear_color = kBg}});
    app.add_plugin<ast::RulesPlugin>();
    load_fx(app);
    app.add_startup(setup, "asteroids.setup");
    app.add_system({.stage = Stage::Render, .fn = draw_world, .name = "asteroids.draw_world",
                    .run_if = &has_local<ast::Run>});
    app.add_system({.stage = Stage::RenderUI, .fn = draw_hud, .name = "asteroids.draw_hud",
                    .run_if = &has_local<ast::Run>});
    if (app.options().has("--bot")) app.res<InputSystem>().set_driver(&bot);
    const int code = app.run();
    if (app.options().headless) {
        World &w = app.main_world();
        const ast::Run *run = w.resources().try_get<ast::Run>();
        const ast::Run r = run ? *run : ast::Run{};
        std::printf("asteroids summary: score=%d best=%d wave=%d lives=%d kills=%u deaths=%u games=%u"
                    " rocks=%zu ticks=%" PRIu64 " hash=%016" PRIx64 "\n",
                    r.score, r.best, r.wave, r.lives, r.kills, r.deaths, r.games, w.count<ast::Rock>(),
                    app.tick(), w.state_hash());
    }
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
