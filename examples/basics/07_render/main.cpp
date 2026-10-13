// render_demo: lam_render in one screen. Walkers roam a grass field, all drawn
// from one sprite sheet (assets/sprites.png, written by make_assets.py), y-sorted over their
// shadows. The camera follows the leader (interpolated, with look-ahead and a dead zone), stays
// inside the field and shakes every three seconds (the simulation emits ShakeAt). A label and a
// triangle marker (draw_triangle) follow the leader and a HUD line shows the render stats; F3
// toggles the debug overlay (LAM_DEV builds), --overlay starts with it on.
//   render_demo [--sprites N] [--frames N] [--hidden] [--headless] [--overlay]
// --headless runs without a window: the frame is still collected and planned (RenderStats).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/debug.h>
#include <lam/core/fixed_string.h>
#include <lam/core/raylib_platform.h>
#include <lam/ecs/component.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/rng.h>
#include <lam/render/camera.h>
#include <lam/render/camera_math.h>
#include <lam/render/font.h>
#include <lam/render/render_plugin.h>
#include <lam/render/render_queue.h>
#include <lam/render/shapes.h>
#include <lam/render/sprite.h>

struct Walker { // px per tick
    lam::Vec2 velocity;
};
LAM_REFLECT(Walker, velocity)

struct Demo { // App resource
    lam::AssetId<lam::Texture> sheet;
    int sprites = 2000;
    lam::Rect field{0, 0, 1200, 800};
    lam::Entity leader;
};

using namespace lam;

static TextureRegion frame(const Demo &d, int i) { // frames 0..7 walk, 8 = grass
    TextureRegion r;
    r.texture = d.sheet;
    r.source = {static_cast<float>(16 * i), 0, 16, 16};
    r.size = {16, 16};
    return r;
}

static void spawn(Ctx &ctx) { // startup, main world
    Demo &d = ctx.res<Demo>();
    d.sheet = ctx.res<AssetServer>().load<Texture>("sprites.png");
    World &w = ctx.world();
    Rng rng(42);
    Entity leader;
    for (int i = 0; i < d.sprites; ++i) {
        const Entity e = w.create();
        w.add<Transform>(
            e, Transform{{rng.range(16.0f, d.field.w - 16.0f), rng.range(16.0f, d.field.h)}});
        Sprite s;
        s.region = frame(d, i % 8);
        s.tint = Color{static_cast<std::uint8_t>(rng.between(90, 255)),
                       static_cast<std::uint8_t>(rng.between(90, 255)),
                       static_cast<std::uint8_t>(rng.between(90, 255)), 255};
        w.add<Sprite>(e, s);
        w.add<Shadow>(e, Shadow{.radii = {6, 2}});
        w.add<Walker>(e, Walker{{rng.range(-1.2f, 1.2f), rng.range(-0.8f, 0.8f)}});
        if (i == 0) leader = e;
    }
    w.get<Sprite>(leader).tint = kWhite;
    d.leader = leader;
    w.add<Text>(leader, Text{.value = "leader", .offset = {0, -30}, .align = TextAlign::Center});

    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{w.get<Transform>(leader).position});
    w.add<CameraFollow>(cam, CameraFollow{.target = leader,
                                          .half_life = 0.15f,
                                          .lookahead_seconds = 0.4f,
                                          .lookahead_max = 40,
                                          .dead_zone = {-24, -16, 48, 32}});
    w.add<CameraBounds>(cam, CameraBounds{d.field, true});
    w.add<CameraShake>(cam);
}

static void walk(Ctx &ctx) { // Simulate: move, bounce, animate
    const Demo &d = ctx.res<Demo>();
    const std::uint64_t tick = ctx.time().tick;
    ctx.world().view<Transform, Sprite, Walker>().each(
        [&](Entity e, Transform &t, Sprite &s, Walker &wk) {
            t.position += wk.velocity;
            if (t.position.x < 8 || t.position.x > d.field.w - 8) wk.velocity.x = -wk.velocity.x;
            if (t.position.y < 16 || t.position.y > d.field.h) wk.velocity.y = -wk.velocity.y;
            s.region = frame(d, static_cast<int>((tick / 6 + e.index) % 8));
            s.flags = static_cast<std::uint8_t>(wk.velocity.x < 0 ? (kYSort | kFlipX) : kYSort);
        });
}

static void quake(Ctx &ctx) { // Simulate: a jolt every 3 seconds (180 ticks), as a sim event
    if (ctx.time().tick % 180 == 179) ctx.emit(ShakeAt{.trauma = 0.6f});
}

static void draw_field(Ctx &ctx) { // Render: grass under the view
    const Demo &d = ctx.res<Demo>();
    RenderQueue &q = ctx.res<RenderQueue>();
    const Rect area = ctx.local<CameraView>().visible_world;
    for (float y = std::floor(area.y / 16) * 16; y < area.y + area.h; y += 16)
        for (float x = std::floor(area.x / 16) * 16; x < area.x + area.w; x += 16)
            if (d.field.contains({x, y}))
                q.region(frame(d, 8), {x, y}, {1, 1}, 0, {0, 0}, kWhite, kLayerTiles, 0);
}

static void marker(Ctx &ctx) { // Render: a triangle pointing down at the leader
    const Demo &d = ctx.res<Demo>();
    World &w = ctx.world();
    if (!w.alive(d.leader)) return;
    const Vec2 p = interpolated(w, d.leader, ctx.time().alpha).position;
    draw_triangle(ctx.res<RenderQueue>(), p + Vec2{-4, -26}, p + Vec2{4, -26}, p + Vec2{0, -20},
                  Color{255, 220, 90, 255}, kLayerForeground);
}

static void hud(Ctx &ctx) { // RenderUI: one line of stats
    const Font *font = ctx.res<AssetServer>().get<Font>(ctx.res<RenderDefaults>().font);
    if (font == nullptr) return;
    const RenderStats &st = ctx.res<RenderStats>();
    FixedString<96> line;
    format_to(line, "sprites {}  draws {}  quads {}  culled {}  F3 overlay",
              ctx.res<Demo>().sprites, st.draw_calls, st.quads, st.culled);
    TextStyle style;
    style.shadow = Color{0, 0, 0, 160};
    draw_text(ctx.res<RenderQueue>(), *font, line.view(), {4, 4}, style);
}

int main(int argc, char **argv) {
    App app({.title = "render_demo",
             .window_size = {960, 540},
             .virtual_size = {480, 270},
             .audio = false},
            argc, argv);
    const bool headless = app.options().headless;
    if (!headless) app.add_plugin<PlatformPlugin>();
    app.add_plugin<AssetsPlugin>();
    app.add_plugin<RenderPlugin>(RenderConfig{.clear_color = Color{24, 28, 36, 255}});
    if (!headless) app.add_plugin<RaylibRenderPlugin>();
    app.register_component<Walker>();

    Demo &demo = app.insert_resource<Demo>();
    const auto &extra = app.options().extra;
    for (std::size_t i = 0; i + 1 < extra.size(); ++i)
        if (extra[i] == "--sprites") demo.sprites = std::max(1, std::atoi(extra[i + 1].c_str()));
    if (app.options().has("--overlay")) app.res<DebugOverlay>().visible = true;

    app.add_startup(spawn, "demo.spawn");
    app.add_system(Stage::Simulate, walk, "demo.walk");
    app.add_system(Stage::Simulate, quake, "demo.quake");
    app.add_system(Stage::Render, draw_field, "demo.field", -1000);
    app.add_system(Stage::Render, marker, "demo.marker");
    app.add_system(Stage::RenderUI, hud, "demo.hud");
    const int code = app.run();

    const RenderStats &st = app.res<RenderStats>();
    std::printf(
        "render_demo: %llu frames, %d sprites, %u draw calls, %u quads, %u culled, plan %.2f ms\n",
        static_cast<unsigned long long>(app.time().frame + 1), demo.sprites, st.draw_calls,
        st.quads, st.culled, static_cast<double>(st.cpu_plan_ms));
    return code;
}
