// RenderPlugin systems end to end. Headless runs stop after planning, so RenderPlan holds exactly
// what the GPU would draw.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/debug.h>
#include <lam/core/platform.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/render/atlas.h>
#include <lam/render/camera.h>
#include <lam/render/camera_math.h>
#include <lam/render/font.h>
#include <lam/render/lighting.h>
#include <lam/render/passes.h>
#include <lam/render/render_plan.h>
#include <lam/render/render_plugin.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>
#include <lam/render/sprite.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>

namespace {

using namespace lam;

// Hand-driven clock (not --headless), so a frame can run 0 ticks and draw with alpha < 1.
class ManualPlatform final : public Platform {
public:
    double t = 0;
    bool should_close() override { return false; }
    double now_seconds() override { return t; }
    void begin_frame() override {}
    void end_frame() override {}
    WindowInfo window() const override {
        WindowInfo w;
        w.framebuffer = {640, 360};
        w.window = {640, 360};
        w.safe_area = {0, 0, 640, 360};
        return w;
    }
    void set_fullscreen(bool) override {}
    bool has_gpu() const override { return false; }
};

TextureRegion region_of(AssetId<Texture> t, Rect source) {
    TextureRegion r;
    r.texture = t;
    r.source = source;
    r.size = source.size();
    return r;
}

// AssetsPlugin + RenderPlugin on a headless App with one 64x64 test texture.
struct Scene {
    test::HeadlessApp h;
    AssetId<Texture> tex;
    explicit Scene(RenderConfig c = {}) {
        h.add<AssetsPlugin>();
        h.add<RenderPlugin>(c);
        tex = test::add_test_texture(assets(), {64, 64});
    }
    App &app() { return h.app(); }
    World &world() { return app().main_world(); }
    AssetServer &assets() { return app().res<AssetServer>(); }
    const RenderPlan &plan() { return app().res<RenderPlan>(); }
    const RenderStats &stats() { return app().res<RenderStats>(); }
    Entity sprite(World &w, Vec2 pos, std::uint8_t layer = kLayerWorld) {
        const Entity e = w.create();
        w.add<Transform>(e, Transform{pos});
        Sprite s;
        s.region = region_of(tex, {0, 0, 16, 16});
        s.layer = layer;
        w.add<Sprite>(e, s);
        return e;
    }
    Entity sprite(Vec2 pos, std::uint8_t layer = kLayerWorld) {
        return sprite(world(), pos, layer);
    }
};

// The top-left vertex of each planned quad, in draw order.
std::vector<Vec2> corners(const RenderPlan &p) {
    std::vector<Vec2> out;
    for (std::size_t i = 0; i < p.staging.size(); i += 4)
        out.push_back(p.staging[i].pos);
    return out;
}

void move_right(Ctx &ctx) {
    ctx.world().view<Transform, Sprite>().each(
        [](Entity, Transform &t, Sprite &) { t.position.x += 16.0f; });
}

// Moves every sprite 1 px right per tick, wrapping around a 660 px wide strip.
void wander(Ctx &ctx) {
    ctx.world().view<Transform, Sprite>().each([](Entity, Transform &t, Sprite &) {
        t.position.x += 1.0f;
        if (t.position.x > 600.0f) t.position.x -= 660.0f;
    });
}

} // namespace

TEST_CASE("render systems: RenderPlugin registers its systems at their stage, scope and order") {
    Scene s;
    const std::string sched = s.app().schedule_text();
    CAPTURE(sched);
    for (const char *name : {"virtual_layout_update", "atlas_shape_page", "fade_occluders",
                             "sprite_fades", "camera_follow", "camera_shake", "camera_view",
                             "begin_view", "collect_sprites", "draw_lights", "resume_view",
                             "draw_debug", "draw_overlay", "render_plan", "render_clear"}) {
        CAPTURE(name);
        CHECK(sched.find(name) != std::string::npos);
    }
    // Run order: begin_view before collect_sprites before draw_lights; plan before clear.
    CHECK(sched.find("begin_view") < sched.find("collect_sprites"));
    CHECK(sched.find("collect_sprites") < sched.find("draw_lights"));
    CHECK(sched.find("camera_follow") < sched.find("camera_shake"));
    CHECK(sched.find("camera_shake") < sched.find("camera_view"));
    CHECK(sched.find("render_plan") < sched.find("render_clear"));
    // Engine resources.
    CHECK(s.app().try_res<RenderQueue>() != nullptr);
    CHECK(s.app().try_res<RenderStats>() != nullptr);
    CHECK(s.app().try_res<VirtualLayout>() != nullptr);
    CHECK(s.app().try_res<AtlasSet>() != nullptr);
    CHECK(s.app().try_res<Materials>() != nullptr);
    CHECK(s.app().try_res<RenderPasses>() != nullptr);
    CHECK(s.app().try_res<RenderDefaults>() != nullptr);
    CHECK(s.app().try_res<RenderPlan>() != nullptr);
    CHECK(s.app().try_res<ScreenCapture>() != nullptr);
    CHECK(s.world().resources().has<CameraView>());
    CHECK(s.world().resources().has<Lighting>());
    World &other = s.app().create_world("second");
    CHECK(other.resources().has<CameraView>());
    CHECK(other.resources().has<Lighting>());
}

TEST_CASE(
    "render systems: the built-in font is Ready on the shape page and the queue uses that page") {
    Scene s;
    const RenderDefaults &d = s.app().res<RenderDefaults>();
    const Font *font = s.assets().get<Font>(d.font);
    REQUIRE(font != nullptr);
    CHECK(font->glyphs.size() == 96u);
    CHECK(font->line_height == 8.0f);
    CHECK(font->page == s.app().res<AtlasSet>().shape_page());
    s.h.frames(1);
    CHECK(s.app().res<RenderQueue>().shape_texture() == font->page);
    // The glyph cells sit after the built-in block on the page.
    const Glyph *a = font->find(U'A');
    REQUIRE(a != nullptr);
    CHECK(a->source.w == 6.0f);
    CHECK(a->source.h == 8.0f);
}

TEST_CASE("render systems: a sprite becomes one quad at its transform; hidden ones are skipped") {
    Scene s;
    const Entity e = s.sprite({100, 100});
    s.h.frames(1);
    REQUIRE(s.plan().staging.size() == 4u);
    const QuadVertex *v = s.plan().staging.data();
    // Pivot (0.5, 1): the feet are at the position.
    CHECK(v[0].pos == Vec2{92, 84});
    CHECK(v[2].pos == Vec2{108, 100});
    CHECK(v[0].uv == Vec2{0, 0});
    CHECK(v[2].uv == Vec2{16, 16});
    CHECK(s.stats().quads == 1u);
    CHECK(s.stats().batches == 1u);
    CHECK(s.stats().views == 1u);
    REQUIRE(s.plan().batches.size() == 1u);
    CHECK(s.plan().batches[0].texture == texture_slot(s.tex));
    CHECK_FALSE(s.plan().batches[0].screen_space);

    s.world().get<Sprite>(e).flags |= kHidden;
    s.h.frames(1);
    CHECK(s.plan().staging.empty());
    CHECK(s.stats().quads == 0u);
}

TEST_CASE("render systems: RenderOffset moves and scales the drawn sprite, not the Transform") {
    Scene s;
    const Entity e = s.sprite({100, 100});
    s.world().add<RenderOffset>(e, RenderOffset{{5, -3}, {2, 2}, 0});
    s.h.frames(1);
    REQUIRE(s.plan().staging.size() == 4u);
    CHECK(s.plan().staging[0].pos == Vec2{105 - 16, 97 - 32});
    CHECK(s.plan().staging[2].pos == Vec2{105 + 16, 97});
    CHECK(s.world().get<Transform>(e).position == Vec2{100, 100});
}

TEST_CASE("render systems: sprites outside the visible world are culled and counted") {
    SUBCASE("culling on (default)") {
        Scene s;
        s.sprite({100, 100});
        s.sprite({5000, 100});
        s.sprite({-30, 100}); // its right edge (-22) is off screen
        s.sprite({-4, 100});  // half visible: kept
        s.h.frames(1);
        CHECK(s.stats().quads == 2u);
        CHECK(s.stats().culled == 2u);
    }
    SUBCASE("RenderConfig::cull = false keeps everything") {
        RenderConfig c;
        c.cull = false;
        Scene s(c);
        s.sprite({100, 100});
        s.sprite({5000, 100});
        s.h.frames(1);
        CHECK(s.stats().quads == 2u);
        CHECK(s.stats().culled == 0u);
    }
    SUBCASE("screen layers cull against the virtual screen, whatever the camera") {
        Scene s;
        const Entity cam = s.world().create();
        s.world().add<Camera2D>(cam, Camera2D{{5000, 5000}});
        s.sprite({100, 100}, kLayerUi);
        s.sprite({100, 100}, kLayerWorld); // world layer, far from the camera
        s.h.frames(1);
        CHECK(s.stats().quads == 1u);
        CHECK(s.stats().culled == 1u);
        REQUIRE(s.plan().batches.size() == 1u);
        CHECK(s.plan().batches[0].screen_space);
    }
}

TEST_CASE("render systems: a sprite whose texture is not loaded is skipped and counted, not drawn "
          "white") {
    Scene s;
    const Entity e = s.sprite({100, 100});
    s.world().get<Sprite>(e).region.texture = AssetId<Texture>{4000, 1};
    s.sprite({120, 100});
    s.h.frames(1);
    CHECK(s.stats().quads == 1u);
    CHECK(s.stats().skipped_unloaded == 1u);
    // Slot 0 (no texture) is the built-in image: always drawable.
    s.world().get<Sprite>(e).region.texture = {};
    s.h.frames(1);
    CHECK(s.stats().quads == 2u);
    CHECK(s.stats().skipped_unloaded == 0u);
}

TEST_CASE(
    "render systems: SpriteFade multiplies the tint's alpha; fully faded sprites are not drawn") {
    Scene s;
    const Entity e = s.sprite({100, 100});
    s.world().add<SpriteFade>(e, SpriteFade{0.5f, 0.5f, 0.2f});
    s.h.frames(1);
    REQUIRE(s.plan().staging.size() == 4u);
    CHECK(s.plan().staging[0].color.a == 128);
    s.world().get<SpriteFade>(e) = SpriteFade{0.0f, 0.0f, 0.2f};
    s.h.frames(1);
    CHECK(s.plan().staging.empty());
}

TEST_CASE("render systems: sprite_fades moves alpha toward its target in frame time") {
    Scene s;
    const Entity e = s.sprite({100, 100});
    s.h.frames(1); // the first frame has dt 0
    s.world().add<SpriteFade>(e, SpriteFade{1.0f, 0.0f, 0.5f});
    s.h.frames(15); // 0.25 s at 60 fps
    CHECK(s.world().get<SpriteFade>(e).alpha == doctest::Approx(0.5f).epsilon(0.02));
    s.h.frames(30);
    CHECK(s.world().get<SpriteFade>(e).alpha == 0.0f);
}

TEST_CASE("render systems: an occluder fades while a revealer stands behind it, then comes back") {
    Scene s;
    World &w = s.world();
    const Entity tree = s.sprite({200, 150});
    w.add<FadeOccluder>(tree, FadeOccluder{{-16, -48, 32, 40}, 2, 0.35f});
    const Entity player = w.create();
    w.add<Transform>(player, Transform{{100, 100}});
    w.add<FadeRevealer>(player);
    s.h.frames(2);
    REQUIRE(w.has<SpriteFade>(tree)); // added through commands
    CHECK(w.get<SpriteFade>(tree).target == 1.0f);
    w.get<Transform>(player).position = {200, 130}; // behind the trunk, inside the canopy
    w.get<PrevTransform>(player).position = {200, 130};
    s.h.frames(30);
    CHECK(w.get<SpriteFade>(tree).target == doctest::Approx(0.35f));
    CHECK(w.get<SpriteFade>(tree).alpha == doctest::Approx(0.35f));
    w.get<Transform>(player).position = {200, 160}; // in front
    w.get<PrevTransform>(player).position = {200, 160};
    s.h.frames(30);
    CHECK(w.get<SpriteFade>(tree).alpha == 1.0f);
}

TEST_CASE("render systems: shadows draw one layer below their sprite; text labels draw glyphs") {
    Scene s;
    World &w = s.world();
    const Entity e = s.sprite({100, 100});
    w.add<Shadow>(e, Shadow{});
    s.h.frames(1);
    REQUIRE(s.stats().quads == 2u);
    // The shadow (layer 63) draws before the sprite (layer 64) although its feet are lower.
    const RenderPlan &p = s.plan();
    CHECK(p.staging[0].color == Color{0, 0, 0, 90});
    CHECK(p.staging[4].color == kWhite);

    const Entity label = w.create();
    w.add<Transform>(label, Transform{{50, 50}});
    Text t;
    t.value = "Hi!";
    w.add<Text>(label, t);
    s.h.frames(1);
    CHECK(s.stats().quads == 2u + 3u);
    // Text on a screen layer is placed at the entity's screen position (identity view here).
    bool found = false;
    for (const Vec2 c : corners(s.plan()))
        found = found || (c.x == 50.0f && c.y == 50.0f);
    CHECK(found);
    // Empty text draws nothing; a hidden sprite hides its shadow too.
    w.get<Text>(label).value = "";
    w.get<Sprite>(e).flags |= kHidden;
    s.h.frames(1);
    CHECK(s.stats().quads == 0u);
}

TEST_CASE("render systems: y-sorted sprites with equal feet keep the entity-index order, whatever "
          "the dense order") {
    Scene s;
    World &w = s.world();
    std::vector<Entity> es;
    for (int i = 0; i < 8; ++i) {
        es.push_back(s.sprite({40.0f + 8.0f * static_cast<float>(i), 100}));
        w.get<Sprite>(es.back()).tint = Color{static_cast<std::uint8_t>(i), 0, 0, 255};
    }
    s.h.frames(1);
    std::vector<int> first;
    for (std::size_t i = 0; i < s.plan().staging.size(); i += 4)
        first.push_back(s.plan().staging[i].color.r);
    CHECK(first == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7});
    // Re-adding sprites reshuffles the dense order of the pool; the drawn order stays put.
    for (int i : {2, 0, 5}) {
        const Sprite keep = w.get<Sprite>(es[static_cast<std::size_t>(i)]);
        w.remove<Sprite>(es[static_cast<std::size_t>(i)]);
        w.add<Sprite>(es[static_cast<std::size_t>(i)], keep);
    }
    s.h.frames(1);
    std::vector<int> second;
    for (std::size_t i = 0; i < s.plan().staging.size(); i += 4)
        second.push_back(s.plan().staging[i].color.r);
    CHECK(second == first);
}

TEST_CASE("render systems: the same scene plans byte-identical frames in two apps") {
    const auto build = [](Scene &s) {
        for (int i = 0; i < 500; ++i) {
            const Entity e =
                s.sprite({static_cast<float>((i * 37) % 480), static_cast<float>((i * 53) % 270)});
            if (i % 7 == 0) s.world().add<Shadow>(e, Shadow{});
            if (i % 10 == 0) s.world().get<Transform>(e).rotation = 0.3f;
        }
        const Entity cam = s.world().create();
        s.world().add<Camera2D>(cam, Camera2D{{200, 120}});
        s.world().add<CameraShake>(cam, CameraShake{0.8f});
        s.h.frames(10);
    };
    Scene a, b;
    build(a);
    build(b);
    REQUIRE(a.plan().staging.size() == b.plan().staging.size());
    REQUIRE(a.plan().batches.size() == b.plan().batches.size());
    CHECK(std::memcmp(a.plan().staging.data(), b.plan().staging.data(),
                      a.plan().staging.size() * sizeof(QuadVertex)) == 0);
    CHECK(a.world().resources().get<CameraView>().center ==
          b.world().resources().get<CameraView>().center);
}

TEST_CASE("render systems: camera_view picks the highest priority camera, then the lowest index") {
    Scene s;
    World &w = s.world();
    const Entity a = w.create();
    w.add<Camera2D>(a, Camera2D{{100, 100}});
    const Entity b = w.create();
    w.add<Camera2D>(b, Camera2D{{300, 100}});
    s.h.frames(1);
    const CameraView &v = w.resources().get<CameraView>();
    CHECK(v.valid);
    CHECK(v.center == Vec2{100, 100});
    w.get<Camera2D>(b).priority = 1;
    s.h.frames(1);
    CHECK(v.center == Vec2{300, 100});
    CHECK(v.visible_world == Rect{60, -35, 480, 270});
    // screen <-> world are inverses and the centre is in the middle of the virtual screen.
    CHECK(world_to_screen(v, {300, 100}) == Vec2{240, 135});
    CHECK(screen_to_world(v, {0, 0}) == Vec2{60, -35});
    const Vec2 p{123.0f, 45.0f};
    CHECK(screen_to_world(v, world_to_screen(v, p)).x == doctest::Approx(p.x));
    CHECK(screen_to_world(v, world_to_screen(v, p)).y == doctest::Approx(p.y));
}

TEST_CASE("render systems: zoom snaps to n or 1/n under PixelPerfect, not under Fit") {
    SUBCASE("pixel perfect") {
        Scene s;
        const Entity c = s.world().create();
        s.world().add<Camera2D>(c, Camera2D{{0, 0}, 1.7f});
        s.h.frames(1);
        const CameraView &v = s.world().resources().get<CameraView>();
        CHECK(v.zoom == 2.0f);
        CHECK(v.visible_world.w == 240.0f);
        s.world().get<Camera2D>(c).zoom = 0.4f;
        s.h.frames(1);
        CHECK(v.zoom == doctest::Approx(1.0f / 3.0f));
        s.world().get<Camera2D>(c).zoom = -2.0f; // nonsense -> 1
        s.h.frames(1);
        CHECK(v.zoom == 1.0f);
    }
    SUBCASE("fit keeps a fractional zoom") {
        RenderConfig cfg;
        cfg.scale_mode = ScaleMode::Fit;
        Scene s(cfg);
        const Entity c = s.world().create();
        s.world().add<Camera2D>(c, Camera2D{{0, 0}, 1.5f});
        s.h.frames(1);
        CHECK(s.world().resources().get<CameraView>().zoom == 1.5f);
    }
}

TEST_CASE("render systems: a world without a camera maps world pixels to virtual pixels") {
    Scene s;
    s.h.frames(1);
    const CameraView &v = s.world().resources().get<CameraView>();
    CHECK(v.valid);
    CHECK(v.center == Vec2{240, 135});
    CHECK(world_to_screen(v, {10, 20}) == Vec2{10, 20});
    CHECK(v.visible_world == Rect{0, 0, 480, 270});
}

TEST_CASE("render systems: camera bounds keep the view inside the map") {
    Scene s;
    World &w = s.world();
    const Entity c = w.create();
    w.add<Camera2D>(c, Camera2D{{-500, 9000}});
    w.add<CameraBounds>(c, CameraBounds{{0, 0, 1000, 600}, true});
    s.h.frames(1);
    const CameraView &v = w.resources().get<CameraView>();
    CHECK(v.center == Vec2{240, 465});
    CHECK(v.visible_world == Rect{0, 330, 480, 270});
}

TEST_CASE("render systems: camera_shake decays trauma and offsets the view by whole pixels") {
    Scene s;
    World &w = s.world();
    const Entity c = w.create();
    w.add<Camera2D>(c, Camera2D{{240, 135}});
    w.add<CameraShake>(c, CameraShake{1.0f});
    bool moved = false;
    for (int i = 0; i < 20; ++i) {
        s.h.frames(1);
        const CameraView &v = w.resources().get<CameraView>();
        const Vec2 d = v.center - Vec2{240, 135};
        CHECK(d.x == std::round(d.x));
        CHECK(d.y == std::round(d.y));
        CHECK(std::fabs(d.x) <= 5.0f);
        CHECK(std::fabs(d.y) <= 5.0f);
        moved = moved || d != Vec2{};
    }
    CHECK(moved);
    // 19 frames of 1/60 s (the first frame has dt 0) at 2.4 per second.
    CHECK(w.get<CameraShake>(c).trauma ==
          doctest::Approx(1.0f - 2.4f * 19.0f / 60.0f).epsilon(0.01));
    s.h.frames(40);
    CHECK(w.get<CameraShake>(c).trauma == 0.0f);
    CHECK(w.resources().get<CameraView>().center == Vec2{240, 135});
}

TEST_CASE("render systems: camera_follow snaps at first, glides after, and snaps on a teleport") {
    Scene s;
    World &w = s.world();
    const Entity hero = s.sprite({100, 100});
    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{{0, 0}});
    CameraFollow f;
    f.target = hero;
    f.half_life = 0.1f;
    w.add<CameraFollow>(cam, f);
    s.h.frames(1);
    CHECK(w.get<Camera2D>(cam).position == Vec2{100, 100}); // the first frame snaps
    REQUIRE(w.has<CameraFollowState>(cam));
    CHECK(w.get<CameraFollowState>(cam).tracking);

    w.get<Transform>(hero).position = {140, 100};
    w.get<PrevTransform>(hero).position = {140, 100};
    s.h.frames(1);
    const float x1 = w.get<Camera2D>(cam).position.x;
    CHECK(x1 > 100.0f);
    CHECK(x1 < 140.0f); // glides
    s.h.frames(120);
    CHECK(w.get<Camera2D>(cam).position.x == doctest::Approx(140.0f).epsilon(0.001));

    w.teleport(hero, {2000, 100}); // beyond snap_distance
    s.h.frames(1);
    CHECK(w.get<Camera2D>(cam).position == Vec2{2000, 100});

    w.get<CameraFollow>(cam).snap_next = true;
    w.get<Transform>(hero).position = {2050, 120};
    w.get<PrevTransform>(hero).position = {2050, 120};
    s.h.frames(1);
    CHECK(w.get<Camera2D>(cam).position == Vec2{2050, 120});
    CHECK_FALSE(w.get<CameraFollow>(cam).snap_next);

    // A dead target leaves the camera where it is.
    w.destroy(hero);
    s.h.frames(3);
    CHECK(w.get<Camera2D>(cam).position == Vec2{2050, 120});
}

TEST_CASE("render systems: sprites and the camera follow the interpolated transform") {
    AppConfig cfg;
    cfg.settings_file = "";
    App app(cfg);
    auto platform = std::make_unique<ManualPlatform>();
    ManualPlatform *clock = platform.get();
    app.set_platform(std::move(platform));
    app.add_plugin<AssetsPlugin>();
    app.add_plugin<RenderPlugin>();
    app.add_system(Stage::Simulate, move_right, "test.move_right");
    World &w = app.main_world();
    const AssetId<Texture> tex = test::add_test_texture(app.res<AssetServer>(), {16, 16});
    const Entity hero = w.create();
    w.add<Transform>(hero, Transform{{100, 100}});
    Sprite sp;
    sp.region = region_of(tex, {0, 0, 16, 16});
    w.add<Sprite>(hero, sp);
    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{{100, 100}});
    CameraFollow f;
    f.target = hero;
    f.half_life = 0; // locked: the camera shows exactly what it follows
    w.add<CameraFollow>(cam, f);

    const auto drawn_x = [&] {
        const RenderPlan &p = app.res<RenderPlan>();
        REQUIRE(p.staging.size() == 4u);
        // Pivot x 0.5: the quad's left edge is 8 px left of the drawn position.
        return p.staging[0].pos.x + 8.0f;
    };
    clock->t = 0.0;
    app.run_frames(1); // dt 0: no tick
    CHECK(drawn_x() == doctest::Approx(100.0f));
    clock->t = 1.0 / 60.0;
    app.run_frames(1); // one tick: 100 -> 116, nothing banked (alpha 0)
    CHECK(w.get<Transform>(hero).position.x == 116.0f);
    CHECK(drawn_x() == doctest::Approx(100.0f));
    CHECK(w.get<Camera2D>(cam).position.x == doctest::Approx(100.0f));
    clock->t = 1.0 / 60.0 + 1.0 / 120.0;
    app.run_frames(1); // no tick, half a tick banked: drawn half way
    CHECK(app.time().alpha == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(drawn_x() == doctest::Approx(108.0f).epsilon(0.001));
    CHECK(w.get<Camera2D>(cam).position.x == doctest::Approx(108.0f).epsilon(0.001));
    // So the hero stays in the middle of the screen while the camera follows it.
    const CameraView &v = w.resources().get<CameraView>();
    CHECK(world_to_screen(v, {108, 100}).x == doctest::Approx(240.0f).epsilon(0.01));
}

TEST_CASE("render systems: each visible world gets its own view, bottom first") {
    Scene s;
    World &top = s.app().create_world("hud");
    const Entity cam = s.world().create();
    s.world().add<Camera2D>(cam, Camera2D{{1000, 1000}});
    s.sprite({1000, 1000});
    s.sprite(top, {100, 100});
    const WorldEntry stack[] = {{&s.world()}, {&top}};
    s.app().set_world_stack(stack);
    s.h.frames(1);
    CHECK(s.stats().views == 2u);
    CHECK(s.stats().quads == 2u);
    REQUIRE(s.plan().batches.size() == 2u);
    CHECK(s.plan().batches[0].view == 0);
    CHECK(s.plan().batches[1].view == 1);
    CHECK(s.world().resources().get<CameraView>().view_id == 0);
    CHECK(top.resources().get<CameraView>().view_id == 1);
    // A world that is not visible draws nothing.
    const WorldEntry hidden[] = {{&s.world()}, {&top, true, false, true}};
    s.app().set_world_stack(hidden);
    s.h.frames(1);
    CHECK(s.stats().views == 1u);
    CHECK(s.stats().quads == 1u);
}

TEST_CASE("render systems: lights stamp into the light target, composited with multiply") {
    Scene s;
    World &w = s.world();
    s.sprite({100, 100});
    Lighting &lighting = w.resources().get<Lighting>();
    s.h.frames(1);
    CHECK(s.app().res<RenderPasses>().find(kLightTargetName) == 0); // not until enabled
    lighting.enabled = true;
    lighting.ambient = Color{40, 40, 80, 255};
    const Entity lamp = w.create();
    w.add<Transform>(lamp, Transform{{120, 100}});
    w.add<Light2D>(lamp, Light2D{});
    const Entity far = w.create();
    w.add<Transform>(far, Transform{{5000, 100}});
    w.add<Light2D>(far, Light2D{});
    s.h.frames(1);
    const RenderPasses &passes = s.app().res<RenderPasses>();
    const RenderTargetId light = passes.find(kLightTargetName);
    REQUIRE(light != 0);
    CHECK(passes.target_of(kLayerLights) == light);
    CHECK(passes.desc(light).filter == TextureFilter::Bilinear);
    CHECK(s.stats().quads == 3u); // sprite, ambient, one lamp (the far one is culled)
    CHECK(s.stats().culled == 1u);
    bool lamp_batch = false, composite = false;
    for (const Batch &b : s.plan().batches) {
        if (b.target == light && b.material == kMaterialAdditive && b.quad_count == 1)
            lamp_batch = true;
        if (b.composite_source == light) {
            composite = true;
            CHECK(b.material == kMaterialMultiply);
            CHECK(b.target == 0);
        }
    }
    CHECK(lamp_batch);
    CHECK(composite);
    CHECK(s.stats().passes >= 2u);
    // Switched off again: no light quads, no composite.
    lighting.enabled = false;
    s.h.frames(1);
    CHECK(s.stats().quads == 1u);
    for (const Batch &b : s.plan().batches)
        CHECK(b.composite_source == 0);
}

TEST_CASE("render systems: RenderConfig::lighting creates the light pass at build") {
    RenderConfig c;
    c.lighting = true;
    Scene s(c);
    CHECK(s.app().res<RenderPasses>().find(kLightTargetName) != 0);
}

#if LAM_DEV
TEST_CASE("render systems: draw_debug draws the world's DebugDraw on top, and it is cleared after "
          "the frame") {
    Scene s;
    World &w = s.world();
    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{{1000, 1000}});
    DebugDraw &dd = w.resources().get<DebugDraw>();
    dd.line({1000, 1000}, {1010, 1000}, Color{255, 0, 0, 255}); // world: through the camera
    dd.rect({10, 10, 20, 20}, Color{0, 255, 0, 255}, true);     // screen: 4 lines
    dd.circle({1000, 1000}, 8, Color{0, 0, 255, 255});          // 20 segments
    dd.text({5, 5}, "dbg", kWhite, true);                       // 3 glyphs
    s.h.frames(1);
    CHECK(s.stats().quads == 1u + 4u + 20u + 3u);
    CHECK(dd.prims().empty());
    for (const Batch &b : s.plan().batches)
        CHECK(b.screen_space);
    // The world line was converted to screen space: it starts at the screen centre.
    bool centre = false;
    for (std::size_t i = 0; i < s.plan().staging.size(); i += 4)
        for (int k = 0; k < 4; ++k)
            centre = centre || std::fabs(s.plan().staging[i + static_cast<std::size_t>(k)].pos.x -
                                         240.0f) < 1.0f;
    CHECK(centre);
    s.h.frames(1);
    CHECK(s.stats().quads == 0u);
}

TEST_CASE("render systems: the F3 overlay draws a panel of stats on top of every world") {
    Scene s;
    s.sprite({100, 100});
    s.h.frames(2);
    CHECK(s.stats().quads == 1u);
    s.app().res<DebugOverlay>().visible = true;
    s.app().res<DebugOverlay>().stat("test.value", 42.0);
    s.h.frames(1);
    CHECK(s.stats().quads > 50u); // panel + glyphs of 5+ lines
    const Batch &last = s.plan().batches.back();
    CHECK(last.screen_space);
    s.app().res<DebugOverlay>().visible = false;
    s.h.frames(1);
    CHECK(s.stats().quads == 1u);
}
#endif

TEST_CASE("render systems: RenderStats and RenderPlan describe the planned frame") {
    Scene s;
    for (int i = 0; i < 10; ++i)
        s.sprite({20.0f * static_cast<float>(i), 100});
    const Entity ui = s.sprite({10, 10}, kLayerUi);
    s.world().get<Sprite>(ui).region.texture = {}; // slot 0: another texture
    s.h.frames(3);
    const RenderStats &st = s.stats();
    CHECK(st.quads == 11u);
    CHECK(st.batches == 2u);
    CHECK(st.draw_calls == 2u); // headless: the planned batches
    CHECK(st.texture_switches == 1u);
    CHECK(st.views == 1u);
    CHECK(st.passes == 1u);
    CHECK(st.cpu_plan_ms >= 0.0f);
    CHECK(st.cpu_collect_ms >= 0.0f);
    CHECK(s.plan().frame == s.app().time().frame);
    CHECK(s.plan().staging.size() == 44u);
    // The queue is cleared at FrameEnd +9000; the plan stays for inspection.
    CHECK(s.app().res<RenderQueue>().size() == 0u);
}

TEST_CASE("render systems: the virtual layout follows the platform framebuffer and scale mode") {
    Scene s;
    s.h.frames(1);
    const VirtualLayout &l = s.app().res<VirtualLayout>();
    CHECK(l.framebuffer == IVec2{640, 360});
    CHECK(l.virtual_size == IVec2{480, 270});
    CHECK(l.scale == 1);
    CHECK(l == compute_virtual_layout({640, 360}, {480, 270}, ScaleMode::PixelPerfect));
}

TEST_CASE("render systems: steady state allocates nothing per frame (sprites, text, shadows, "
          "lights, debug)") {
    Scene s;
    World &w = s.world();
    w.reserve<Transform>(2048);
    w.reserve<PrevTransform>(2048);
    w.reserve<Sprite>(2048);
    for (int i = 0; i < 1500; ++i) {
        const Entity e = s.sprite({static_cast<float>((i * 37) % 600) - 60.0f,
                                   static_cast<float>((i * 53) % 330) - 30.0f});
        if (i % 10 == 0) w.get<Transform>(e).rotation = 0.5f;
        if (i % 25 == 0) w.add<Shadow>(e, Shadow{});
        if (i % 50 == 0) w.add<SpriteFade>(e, SpriteFade{0.5f, 1.0f, 2.0f});
    }
    s.app().add_system(Stage::Simulate, wander, "test.wander");
    const Entity tree = s.sprite({200, 150});
    w.add<FadeOccluder>(tree, FadeOccluder{{-16, -48, 32, 40}});
    const Entity player = w.create();
    w.add<Transform>(player, Transform{{200, 130}});
    w.add<FadeRevealer>(player);
    Text t;
    t.value = "score 12345";
    w.add<Text>(player, t);
    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{{240, 135}});
    w.add<CameraShake>(cam, CameraShake{0.5f, 0.1f});
    CameraFollow f;
    f.target = player;
    f.lookahead_seconds = 0.2f;
    f.lookahead_max = 24;
    w.add<CameraFollow>(cam, f);
    w.resources().get<Lighting>().enabled = true;
    const Entity lamp = w.create();
    w.add<Transform>(lamp, Transform{{220, 140}});
    w.add<Light2D>(lamp, Light2D{.flicker = 0.3f});
    s.app().res<DebugOverlay>().visible = true;
    const auto debug_draws = [&] {
        for (int i = 0; i < 30; ++i) {
            w.resources().get<DebugDraw>().line({0, 0}, {50, 50}, Color{255, 0, 0, 255});
            w.resources().get<DebugDraw>().circle({100, 100}, 8, Color{0, 0, 255, 255});
            test::run_ticks(s.app(), 1);
        }
    };
    test::run_ticks(s.app(), 120);
    debug_draws();
    CHECK(s.stats().quads > 1000u);
    CHECK_NO_ALLOC(test::run_ticks(s.app(), 600));
    CHECK_NO_ALLOC(debug_draws());
    // Canary: growing the queue past its capacity must be counted.
    if (allocation_counting_enabled()) {
        MUST_FAIL(CHECK_NO_ALLOC(for (int i = 0; i < 70000; ++i) s.app().res<RenderQueue>().rect(
            {0, 0, 1, 1}, kWhite, kLayerUi)));
    }
    s.app().res<RenderQueue>().clear();
}

TEST_CASE("render systems: a sprite without PrevTransform still draws while interpolating, at its "
          "Transform") {
    AppConfig cfg;
    cfg.settings_file = "";
    App app(cfg);
    auto platform = std::make_unique<ManualPlatform>();
    ManualPlatform *clock = platform.get();
    app.set_platform(std::move(platform));
    app.add_plugin<AssetsPlugin>();
    app.add_plugin<RenderPlugin>();
    app.add_system(Stage::Simulate, move_right, "test.move_right");
    World &w = app.main_world();
    const AssetId<Texture> tex = test::add_test_texture(app.res<AssetServer>(), {16, 16});
    const Entity e = w.create();
    w.add<Transform>(e, Transform{{100, 100}});
    Sprite sp;
    sp.region = region_of(tex, {0, 0, 16, 16});
    w.add<Sprite>(e, sp);
    clock->t = 0.0;
    app.run_frames(1);
    clock->t = 1.0 / 60.0;
    app.run_frames(1); // one tick: 100 -> 116, spawn settled
    REQUIRE_FALSE(w.has<JustSpawned>(e));
    w.remove<PrevTransform>(e); // opted out of interpolation
    clock->t = 1.0 / 60.0 + 1.0 / 120.0;
    app.run_frames(1); // no tick, alpha 0.5
    REQUIRE(app.time().alpha == doctest::Approx(0.5f).epsilon(0.001));
    const RenderPlan &p = app.res<RenderPlan>();
    REQUIRE(p.staging.size() == 4u); // drawn, like interpolated() reports it: at the Transform
    CHECK(p.staging[0].pos.x + 8.0f == doctest::Approx(116.0f));
    CHECK(interpolated(w, e, 0.5f).position.x == 116.0f);
}
