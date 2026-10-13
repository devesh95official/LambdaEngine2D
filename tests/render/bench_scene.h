#pragma once
// Bench scene for the render benches and gpu tests: animated 16x16 sprites (half moving, 10%
// rotated), 3 tile layers over the 480x270 view, 400 additive particles and a UI panel.
// Everything sits on the first atlas page, so the scene uses a single texture.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/fixed_string.h>
#include <lam/ecs/component.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/rng.h>
#include <lam/math/scalar.h>
#include <lam/render/atlas.h>
#include <lam/render/camera.h>
#include <lam/render/font.h>
#include <lam/render/render_plan.h>
#include <lam/render/render_plugin.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>
#include <lam/render/sprite.h>

namespace lam::bench {

struct BenchMove { // bench-local velocity, px per tick
    Vec2 v;
    bool spin = false;
};

struct BenchScene { // App resource
    TextureRegion frames[8];
    TextureRegion tile;
    int sprites = 0;
    Vec2 field{480, 270};
};

// The sheet: 8 frames of a 16x16 walker (a body bobbing over two legs), then one 16x16 tile.
inline ImageData make_sheet() {
    ImageData img;
    img.width = 16 * 9;
    img.height = 16;
    img.rgba.assign(static_cast<std::size_t>(img.width * img.height * 4), 0);
    const auto put = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) +
                               static_cast<std::size_t>(x)) *
                              4;
        img.rgba[i] = r;
        img.rgba[i + 1] = g;
        img.rgba[i + 2] = b;
        img.rgba[i + 3] = 255;
    };
    for (int f = 0; f < 8; ++f) {
        const int ox = f * 16;
        const int bob = (f % 4 == 1 || f % 4 == 2) ? 1 : 0;
        for (int y = 2 + bob; y < 11 + bob; ++y)
            for (int x = 4; x < 12; ++x)
                put(ox + x, y, static_cast<std::uint8_t>(80 + f * 20), 120, 220);
        put(ox + 6, 5 + bob, 255, 255, 255);
        put(ox + 9, 5 + bob, 255, 255, 255);
        const int step = (f < 4) ? f : 7 - f;
        for (int y = 11 + bob; y < 16; ++y) {
            put(ox + 5 + step / 2, y, 60, 40, 30);
            put(ox + 10 - step / 2, y, 60, 40, 30);
        }
    }
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const bool blade = ((x * 7 + y * 3) % 11) == 0;
            put(16 * 8 + x, y, blade ? 90 : 40, blade ? 160 : 110, blade ? 70 : 50);
        }
    return img;
}

inline void move_sprites(Ctx &ctx) {
    const BenchScene &s = ctx.res<BenchScene>();
    const std::uint64_t tick = ctx.time().tick;
    ctx.world().view<Transform, Sprite, BenchMove>().each(
        [&](Entity e, Transform &t, Sprite &sp, BenchMove &m) {
            t.position += m.v;
            if (t.position.x < 0 || t.position.x > s.field.x) m.v.x = -m.v.x;
            if (t.position.y < 8 || t.position.y > s.field.y + 8) m.v.y = -m.v.y;
            if (m.spin) t.rotation += 0.05f;
            sp.region = s.frames[(tick / 6 + e.index) % 8];
        });
}

inline void draw_tiles(Ctx &ctx) {
    const BenchScene &s = ctx.res<BenchScene>();
    RenderQueue &q = ctx.res<RenderQueue>();
    for (int layer = 0; layer < 3; ++layer) {
        const Color tint = layer == 0 ? kWhite : Color{255, 255, 255, 96};
        for (int y = 0; y < 18; ++y)
            for (int x = 0; x < 31; ++x)
                q.region(s.tile, {static_cast<float>(x * 16), static_cast<float>(y * 16)}, {1, 1},
                         0, {0, 0}, tint, static_cast<std::uint8_t>(kLayerTiles + layer), 0);
    }
}

inline void draw_particles(Ctx &ctx) {
    RenderQueue &q = ctx.res<RenderQueue>();
    const float t = static_cast<float>(ctx.time().frame % 600) * 0.01f;
    for (int i = 0; i < 400; ++i) {
        const float a = static_cast<float>(i) * 0.37f + t;
        const Vec2 c{240.0f + 200.0f * det_cos(a) * static_cast<float>(i % 7) / 7.0f,
                     135.0f + 110.0f * det_sin(a * 1.3f) * static_cast<float>(i % 5) / 5.0f};
        q.light(c, 4.0f, Color{255, 180, 80, 160}, kMaterialAdditive, kLayerForeground);
    }
}

inline void draw_ui(Ctx &ctx) {
    RenderQueue &q = ctx.res<RenderQueue>();
    const AssetServer &assets = ctx.res<AssetServer>();
    const Font *font = assets.get<Font>(ctx.res<RenderDefaults>().font);
    q.rect({8, 200, 180, 62}, Color{20, 20, 40, 200}, kLayerUi);
    if (font == nullptr) return;
    const BenchScene &s = ctx.res<BenchScene>();
    const RenderStats &st = ctx.res<RenderStats>();
    FixedString<64> line;
    TextStyle style;
    format_to(line, "sprites {}", s.sprites);
    draw_text(q, *font, line.view(), {14, 206}, style);
    format_to(line, "draws {} quads {}", st.draw_calls, st.quads);
    draw_text(q, *font, line.view(), {14, 216}, style);
    format_to(line, "collect {:.2f} plan {:.2f}", st.cpu_collect_ms, st.cpu_plan_ms);
    draw_text(q, *font, line.view(), {14, 226}, style);
    draw_text(q, *font, "the quick brown fox", {14, 240}, style);
}

// Packs the sheet into the default atlas, registers the scene's systems and spawns `sprites`
// sprites in the 480x270 field (deterministic for a seed) and a camera over it.
inline void setup(App &app, int sprites, std::uint64_t seed = 7) {
    app.register_component<BenchMove>();
    AssetServer &assets = app.res<AssetServer>();
    Result<TextureRegion> sheet = app.res<AtlasSet>().add(assets, make_sheet(), str_id("default"));
    LAM_CHECK(static_cast<bool>(sheet), "bench: sheet does not fit the atlas");
    BenchScene &s = app.insert_resource<BenchScene>();
    s.sprites = sprites;
    for (int f = 0; f < 8; ++f) {
        s.frames[f].texture = sheet->texture;
        s.frames[f].source = {sheet->source.x + static_cast<float>(f * 16), sheet->source.y, 16,
                              16};
        s.frames[f].size = {16, 16};
    }
    s.tile.texture = sheet->texture;
    s.tile.source = {sheet->source.x + 128.0f, sheet->source.y, 16, 16};
    s.tile.size = {16, 16};
    app.add_system(Stage::Simulate, move_sprites, "bench.move");
    app.add_system(Stage::Render, draw_tiles, "bench.tiles", -1000);
    app.add_system(Stage::Render, draw_particles, "bench.particles", -800);
    app.add_system(Stage::RenderUI, draw_ui, "bench.ui", -1000);

    World &w = app.main_world();
    w.reserve<Transform>(static_cast<std::size_t>(sprites) + 16);
    w.reserve<PrevTransform>(static_cast<std::size_t>(sprites) + 16);
    w.reserve<Sprite>(static_cast<std::size_t>(sprites) + 16);
    w.reserve<BenchMove>(static_cast<std::size_t>(sprites) / 2 + 16);
    Rng rng(seed);
    for (int i = 0; i < sprites; ++i) {
        const Entity e = w.create();
        Transform t{{rng.range(0.0f, s.field.x), rng.range(8.0f, s.field.y + 8.0f)}};
        const bool rotated = i % 10 == 0;
        if (rotated) t.rotation = rng.range(0.0f, 6.28f);
        w.add<Transform>(e, t);
        Sprite sp;
        sp.region = s.frames[i % 8];
        w.add<Sprite>(e, sp);
        if (i % 2 == 0)
            w.add<BenchMove>(e,
                             BenchMove{{rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f)}, rotated});
    }
    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{s.field * 0.5f + Vec2{0, 8}});
}

// Sorted copy -> the p-th percentile (0..100) of `v`.
inline float percentile(std::vector<float> v, float p) {
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const std::size_t i =
        static_cast<std::size_t>(p / 100.0f * static_cast<float>(v.size() - 1) + 0.5f);
    return v[std::min(i, v.size() - 1)];
}

} // namespace lam::bench

LAM_REFLECT(lam::bench::BenchMove, v, spin)
