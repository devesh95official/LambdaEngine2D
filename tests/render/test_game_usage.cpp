// Documented render behaviour that games rely on, checked headless.
#include <cmath>
#include <cstdint>
#include <string>

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/data.h>
#include <lam/ecs/component.h>
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
#include <lam/render/shapes.h>
#include <lam/render/sprite.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>

namespace {

using namespace lam;

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
};

// ---- ShakeAt from a tick system ----------------------------------------------------------------

std::uint32_t g_shake_world = 0;
int g_shake_ticks = 0;
float g_shake_amount = 0;

void emit_shake(Ctx &ctx) {
    if (ctx.world().id() != g_shake_world || g_shake_ticks <= 0) return;
    --g_shake_ticks;
    ctx.emit(ShakeAt{g_shake_amount});
    ctx.emit(ShakeAt{g_shake_amount});
}

// The centre of planned quad `q` (staging holds world px for world layers).
Vec2 quad_center(const RenderPlan &p, std::uint32_t q) {
    const QuadVertex *v = p.staging.data() + std::size_t{q} * 4;
    return (v[0].pos + v[2].pos) * 0.5f;
}

} // namespace

TEST_CASE("ShakeAt from the simulation shakes every CameraShake of its world only") {
    Scene s;
    World &w = s.world();
    World &hud = s.app().create_world("hud");
    const WorldEntry stack[] = {{&w}, {&hud}};
    s.app().set_world_stack(stack);
    s.app().add_system(Stage::Simulate, emit_shake, "test.emit_shake");
    g_shake_world = w.id();
    g_shake_ticks = 0;
    g_shake_amount = 0.7f;
    const Entity a = w.create();
    w.add<Camera2D>(a, Camera2D{{240, 135}});
    w.add<CameraShake>(a);
    const Entity b = w.create();
    w.add<CameraShake>(b, CameraShake{0.1f}); // a second shake in the same world
    const Entity other = hud.create();
    hud.add<Camera2D>(other);
    hud.add<CameraShake>(other);
    s.h.frames(1); // dt 0, nothing emitted
    CHECK(w.get<CameraShake>(a).trauma == 0.0f);

    g_shake_ticks = 1; // the next tick emits two events of 0.7
    s.h.frames(1);
    const float decay = 2.4f / 60.0f;
    // 0 + 0.7 + 0.7 clamps to 1, then one frame of decay; added before decaying, so the frame that
    // draws the hit already shakes.
    CHECK(w.get<CameraShake>(a).trauma == doctest::Approx(1.0f - decay));
    CHECK(w.get<CameraShake>(b).trauma == doctest::Approx(1.0f - decay));
    CHECK(hud.get<CameraShake>(other).trauma == 0.0f); // another world's shake is untouched
    s.h.frames(1);                                     // every event is read once
    CHECK(w.get<CameraShake>(a).trauma == doctest::Approx(1.0f - 2.0f * decay));

    // Non-finite and negative amounts change nothing; small ones add up.
    g_shake_amount = std::nanf("");
    g_shake_ticks = 1;
    s.h.frames(1);
    CHECK(w.get<CameraShake>(a).trauma == doctest::Approx(1.0f - 3.0f * decay));
    s.h.frames(60);
    CHECK(w.get<CameraShake>(a).trauma == 0.0f);
    g_shake_amount = 0.1f;
    g_shake_ticks = 1;
    s.h.frames(1);
    CHECK(w.get<CameraShake>(a).trauma == doctest::Approx(0.2f - decay));

    // Steady state: an event every tick allocates nothing.
    g_shake_ticks = 1000;
    s.h.frames(30);
    CHECK_NO_ALLOC(s.h.frames(120));
    CHECK(w.get<CameraShake>(a).trauma ==
          doctest::Approx(1.0f - decay)); // pinned at 1, decayed once

    // A world with ShakeAt events but no CameraShake ignores them.
    g_shake_world = hud.id();
    hud.remove<CameraShake>(other);
    g_shake_ticks = 1;
    s.h.frames(1);
    CHECK_FALSE(hud.has<CameraShake>(other));
}

TEST_CASE("ShakeAt is registered as a World event by RenderPlugin") {
    Scene s;
    // Emitting from a system of any world must not abort (unregistered events do).
    s.app().add_system(Stage::Update, [](Ctx &ctx) { ctx.emit(ShakeAt{}); }, "test.emit_update");
    s.h.frames(3);
    CHECK(s.world().resources().get<CameraView>().valid);
}

TEST_CASE("a follow target set late (or not spawned yet) leaves the camera still, "
          "then snaps") {
    Scene s;
    World &w = s.world();
    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{{50, 60}});
    w.add<CameraFollow>(cam); // no target yet
    s.h.frames(2);
    CHECK(w.get<Camera2D>(cam).position == Vec2{50, 60});
    CHECK_FALSE(w.has<CameraFollowState>(cam));

    // The map spawns the player later (e.g. at FrameBegin): set the target any time.
    const Entity hero = w.create();
    w.add<Transform>(hero, Transform{{300, 200}});
    w.get<CameraFollow>(cam).target = hero;
    s.h.frames(1);
    CHECK(w.get<Camera2D>(cam).position == Vec2{300, 200}); // first frame with a target snaps
}

TEST_CASE("CameraBounds belongs on the camera entity; on the target it does nothing") {
    Scene s;
    World &w = s.world();
    const Entity hero = w.create();
    w.add<Transform>(hero, Transform{{10, 10}});
    w.add<CameraBounds>(hero, CameraBounds{{0, 0, 1000, 1000}, true}); // wrong entity: ignored
    const Entity cam = w.create();
    w.add<Camera2D>(cam);
    CameraFollow f;
    f.target = hero;
    w.add<CameraFollow>(cam, f);
    s.h.frames(1);
    CHECK(w.get<Camera2D>(cam).position == Vec2{10, 10});
    w.add<CameraBounds>(cam, CameraBounds{{0, 0, 1000, 1000}, true});
    s.h.frames(1);
    // The 480x270 view stays inside the bounds (applied by camera_follow and camera_view).
    CHECK(w.get<Camera2D>(cam).position == Vec2{240, 135});
    CHECK(w.resources().get<CameraView>().visible_world == Rect{0, 0, 480, 270});
}

TEST_CASE("aim above the feet by following a child entity (Parent::local)") {
    Scene s;
    World &w = s.world();
    const Entity hero = w.create();
    w.add<Transform>(hero, Transform{{100, 100}});
    const Entity anchor = w.create();
    w.add<Transform>(anchor);
    set_parent(w, anchor, hero, Transform{{0, -8}});
    const Entity cam = w.create();
    w.add<Camera2D>(cam);
    CameraFollow f;
    f.target = anchor;
    f.half_life = 0;
    w.add<CameraFollow>(cam, f);
    s.h.frames(1);
    CHECK(w.get<Camera2D>(cam).position == Vec2{100, 92});
    w.get<Transform>(hero).position = {140, 100}; // the anchor follows at the next propagation
    s.h.frames(2);
    CHECK(w.get<Camera2D>(cam).position.x == doctest::Approx(140.0f));
    CHECK(w.get<Camera2D>(cam).position.y == doctest::Approx(92.0f));
}

TEST_CASE("CameraFollow::offset is added to the aim point") {
    Scene s;
    World &w = s.world();
    const Entity hero = w.create();
    w.add<Transform>(hero, Transform{{100, 100}});
    const Entity cam = w.create();
    w.add<Camera2D>(cam);
    CameraFollow f;
    f.target = hero;
    f.offset = Vec2{0, -8};
    w.add<CameraFollow>(cam, f);
    s.h.frames(1);
    CHECK(w.get<Camera2D>(cam).position == Vec2{100, 92});
    // Reflected: the offset survives a save round trip.
    const ComponentInfo &info = ComponentRegistry::global().info<CameraFollow>();
    SerializeContext ctx;
    DataWriter out;
    info.write(&f, out, ctx);
    Result<std::string> json = out.finish();
    REQUIRE(json);
    Result<DataDocument> doc = DataDocument::parse(json.value(), "follow.json");
    REQUIRE(doc);
    CameraFollow back;
    DataReader root = doc.value().root();
    REQUIRE(info.read(&back, root, ctx));
    CHECK(back.offset == Vec2{0, -8});
}

TEST_CASE("Lighting::enabled alone turns lights on; Light2D::offset moves the light") {
    Scene s; // RenderConfig::lighting stays false
    World &w = s.world();
    CHECK(s.app().res<RenderPasses>().find(kLightTargetName) == 0);
    w.resources().get<Lighting>().enabled = true;
    const Entity lamp = w.create();
    w.add<Transform>(lamp, Transform{{120, 100}});
    Light2D l;
    l.radius = 20;
    l.offset = {10, -20};
    w.add<Light2D>(lamp, l);
    // Far outside the view, but its offset brings the light back in: not culled.
    const Entity pole = w.create();
    w.add<Transform>(pole, Transform{{5000, 100}});
    Light2D l2;
    l2.radius = 10;
    l2.offset = {-4800, 0};
    w.add<Light2D>(pole, l2);
    s.h.frames(1);
    const RenderTargetId light = s.app().res<RenderPasses>().find(kLightTargetName);
    REQUIRE(light != 0); // created on first use
    CHECK(s.stats().culled == 0u);
    int lamps = 0;
    for (const Batch &b : s.plan().batches) {
        if (b.target != light || b.material != kMaterialAdditive) continue;
        for (std::uint32_t q = b.first_quad; q < b.first_quad + b.quad_count; ++q) {
            const Vec2 c = quad_center(s.plan(), q);
            const float r = s.plan().staging[std::size_t{q} * 4 + 2].pos.x - c.x;
            if (r == doctest::Approx(20.0f)) {
                CHECK(c == Vec2{130, 80});
                ++lamps;
            } else {
                CHECK(c == Vec2{200, 100});
                ++lamps;
            }
        }
    }
    CHECK(lamps == 2);
}

TEST_CASE("Light2D::offset is saved (reflected) and defaults to zero") {
    const ComponentInfo &info = ComponentRegistry::global().info<Light2D>();
    Light2D l;
    CHECK(l.offset == Vec2{});
    l.offset = {3, -4};
    DataWriter out;
    info.write(&l, out, SerializeContext{});
    Result<std::string> json = out.finish();
    REQUIRE(json);
    CHECK(json.value().find("\"offset\"") != std::string::npos);
    Result<DataDocument> doc = DataDocument::parse(json.value(), "light.json");
    REQUIRE(doc);
    Light2D back;
    DataReader root = doc.value().root();
    REQUIRE(info.read(&back, root, SerializeContext{}));
    CHECK(back.offset == Vec2{3, -4});
    // Older data without the key keeps the default.
    Result<DataDocument> old = DataDocument::parse(R"({"radius": 12})", "old.json");
    REQUIRE(old);
    Light2D legacy;
    DataReader old_root = old.value().root();
    REQUIRE(info.read(&legacy, old_root, SerializeContext{}));
    CHECK(legacy.radius == 12.0f);
    CHECK(legacy.offset == Vec2{});
}

TEST_CASE("a pivot outside 0..1 draws above the feet and is not culled early") {
    Scene s;
    World &w = s.world();
    // A canopy whose bottom is 50 px above the feet of a 32x32 frame: pivot y = 1 + 50 / 32.
    // Its feet are 40 px below the visible world (no camera: world px = virtual px, 480x270).
    const Entity tree = w.create();
    w.add<Transform>(tree, Transform{{100, 310}});
    Sprite sp;
    sp.region.texture = s.tex;
    sp.region.source = {0, 0, 32, 32};
    sp.region.size = {32, 32};
    sp.pivot = {0.5f, 1.0f + 50.0f / 32.0f};
    w.add<Sprite>(tree, sp);
    s.h.frames(1);
    CHECK(s.stats().culled == 0u);
    REQUIRE(s.plan().staging.size() == 4u);
    CHECK(s.plan().staging[0].pos == Vec2{84, 228});  // top-left
    CHECK(s.plan().staging[2].pos == Vec2{116, 260}); // bottom-right: 50 px above the feet
    // Further down, the whole frame is off screen: culled.
    w.teleport(tree, {100, 400});
    s.h.frames(1);
    CHECK(s.stats().culled == 1u);
}

TEST_CASE("a Sprite on an atlas page saves the page's name and loads back to it") {
    Scene s;
    AtlasSet &atlas = s.app().res<AtlasSet>();
    ImageData img;
    img.width = 16;
    img.height = 16;
    Result<TextureRegion> r = atlas.add(s.assets(), img);
    REQUIRE(r);
    const std::string_view page_name = s.assets().path_of(r.value().texture);
    CHECK(page_name.starts_with("@atlas/default/"));
    Sprite sp;
    sp.region = r.value();
    const ComponentInfo &info = ComponentRegistry::global().info<Sprite>();
    SerializeContext ctx;
    ctx.assets = &s.assets();
    DataWriter out;
    info.write(&sp, out, ctx);
    Result<std::string> json = out.finish();
    REQUIRE(json);
    CHECK(json.value().find(std::string(page_name)) != std::string::npos);
    Result<DataDocument> doc = DataDocument::parse(json.value(), "sprite.json");
    REQUIRE(doc);
    Sprite back;
    DataReader root = doc.value().root();
    REQUIRE(info.read(&back, root, ctx));
    CHECK(back.region.texture == sp.region.texture);
    CHECK(back.region.source == sp.region.source);

    // An unnamed runtime texture has no path: it saves as "" and loads as none (slot 0).
    Texture t;
    t.width = 8;
    t.height = 8;
    const AssetId<Texture> unnamed = s.assets().add<Texture>(std::move(t));
    CHECK(s.assets().path_of(unnamed).empty());
    Sprite lost;
    lost.region.texture = unnamed;
    DataWriter out2;
    info.write(&lost, out2, ctx);
    Result<std::string> json2 = out2.finish();
    REQUIRE(json2);
    Result<DataDocument> doc2 = DataDocument::parse(json2.value(), "lost.json");
    REQUIRE(doc2);
    Sprite lost_back;
    lost_back.region.texture = s.tex;
    DataReader root2 = doc2.value().root();
    (void)info.read(&lost_back, root2, ctx);
    CHECK_FALSE(lost_back.region.texture);
}

TEST_CASE("an overlay world without Camera2D draws in virtual pixels") {
    Scene s;
    World &hud = s.app().create_world("hud");
    const WorldEntry stack[] = {{&s.world()}, {&hud}};
    s.app().set_world_stack(stack);
    const Entity cam = s.world().create();
    s.world().add<Camera2D>(cam, Camera2D{{2000, 2000}, 2.0f});
    s.h.frames(1);
    const CameraView &v = hud.resources().get<CameraView>();
    CHECK(v.valid);
    CHECK(v.center == Vec2{240, 135});
    CHECK(v.zoom == 1.0f);
    CHECK(world_to_screen(v, {10, 20}) == Vec2{10, 20});
    CHECK(v.visible_world == Rect{0, 0, 480, 270});
}

TEST_CASE("TextStyle::size is the pixel size; scale = size / Font::base_size") {
    Scene s;
    const Font *font = s.assets().get<Font>(s.app().res<RenderDefaults>().font);
    REQUIRE(font != nullptr);
    CHECK(font->base_size == 8.0f);                       // the built-in 6x8 font
    CHECK(measure_text(*font, "ab", 0) == Vec2{12, 8});   // 0 = base size
    CHECK(measure_text(*font, "ab", 8) == Vec2{12, 8});   // 1x
    CHECK(measure_text(*font, "ab", 16) == Vec2{24, 16}); // 2x
    CHECK(measure_text(*font, "ab", 24) == Vec2{36, 24}); // 3x
}

TEST_CASE("draw_triangle pushes one quad of the white block with c repeated") {
    RenderQueue q(16);
    const AssetId<Texture> page{3, 1};
    q.set_shape_texture(page);
    const Color red{255, 0, 0, 200};
    draw_triangle(q, {0, 0}, {10, 0}, {5, 8}, red, kLayerUi);
    REQUIRE(q.size() == 1u);
    const DrawQuad &d = q.quads()[0];
    CHECK(d.v[0].pos == Vec2{0, 0});
    CHECK(d.v[1].pos == Vec2{10, 0});
    CHECK(d.v[2].pos == Vec2{5, 8});
    CHECK(d.v[3].pos == Vec2{5, 8});
    for (const QuadVertex &v : d.v) {
        CHECK(v.color == red);
        CHECK(v.uv.x >= kWhiteSource.x);
        CHECK(v.uv.x <= kWhiteSource.x + kWhiteSource.w);
        CHECK(v.uv.y >= kWhiteSource.y);
        CHECK(v.uv.y <= kWhiteSource.y + kWhiteSource.h);
    }
    CHECK(key_layer(d.key) == kLayerUi);
    CHECK(key_texture(d.key) == texture_slot(page));
    CHECK(key_material(d.key) == 0);
    // Counter-clockwise works the same (no culling); degenerate and non-finite draw nothing.
    draw_triangle(q, {0, 0}, {5, 8}, {10, 0}, red, kLayerWorld, 3.0f);
    CHECK(q.size() == 2u);
    draw_triangle(q, {0, 0}, {5, 5}, {10, 10}, red, kLayerWorld);
    draw_triangle(q, {0, 0}, {0, 0}, {10, 10}, red, kLayerWorld);
    draw_triangle(q, {std::nanf(""), 0}, {5, 5}, {10, 0}, red, kLayerWorld);
    draw_triangle(q, {INFINITY, 0}, {5, 5}, {10, 0}, red, kLayerWorld);
    CHECK(q.size() == 2u);
    // RenderQueue::triangle forwards to draw_triangle.
    q.triangle({0, 0}, {10, 0}, {5, 8}, red, kLayerUi);
    REQUIRE(q.size() == 3u);
    const DrawQuad first = q.quads()[0];
    const DrawQuad &t = q.quads()[2];
    for (int i = 0; i < 4; ++i) {
        CHECK(t.v[i].pos == first.v[i].pos);
        CHECK(t.v[i].uv == first.v[i].uv);
        CHECK(t.v[i].color == red);
    }
    CHECK(key_layer(t.key) == kLayerUi);
    CHECK(key_texture(t.key) == texture_slot(page));
    q.triangle({0, 0}, {0, 0}, {10, 10}, red, kLayerWorld);
    CHECK(q.size() == 3u);
}

TEST_CASE("triangles batch with sprites and shapes of the shape page") {
    Scene s;
    s.app().add_system(
        Stage::Render,
        [](Ctx &ctx) {
            RenderQueue &q = ctx.res<RenderQueue>();
            q.rect({0, 0, 10, 10}, kWhite, kLayerWorld);
            draw_triangle(q, {20, 0}, {30, 0}, {25, 10}, kWhite, kLayerWorld);
            q.circle({50, 5}, 5, kWhite, kLayerWorld);
            draw_triangle(q, {0, 0}, {8, 0}, {4, 6}, kWhite, kLayerUi);
        },
        "test.shapes");
    s.h.frames(1);
    CHECK(s.stats().quads == 4u);
    // One batch for the world layer, one for the screen layer (screen space breaks a batch).
    CHECK(s.stats().batches == 2u);
    s.h.frames(30);
    CHECK_NO_ALLOC(s.h.frames(60));
}

TEST_CASE("light and circle quads sample half a texel inside their cell") {
    RenderQueue q{16};
    q.light({100, 100}, 56, kWhite, MaterialId{}, kLayerLights);
    q.ellipse({40, 40}, {7, 5}, kWhite, kLayerWorld);
    REQUIRE(q.quads().size() == 2);
    const auto inside = [](const DrawQuad &d, const Rect &cell) {
        for (const QuadVertex &v : d.v) {
            CHECK(v.uv.x >= cell.x + 0.5f);
            CHECK(v.uv.x <= cell.x + cell.w - 0.5f);
            CHECK(v.uv.y >= cell.y + 0.5f);
            CHECK(v.uv.y <= cell.y + cell.h - 0.5f);
        }
    };
    const DrawQuad &light = key_layer(q.quads()[0].key) == kLayerLights ? q.quads()[0] : q.quads()[1];
    const DrawQuad &circle = key_layer(q.quads()[0].key) == kLayerLights ? q.quads()[1] : q.quads()[0];
    inside(light, kFalloffSource);
    inside(circle, kCircleSource);
    // The quads still cover the full light and ellipse rects.
    CHECK(light.v[0].pos.x == 44.0f);
    CHECK(light.v[2].pos.x == 156.0f);
    CHECK(circle.v[0].pos.x == 33.0f);
    CHECK(circle.v[2].pos.y == 45.0f);
}
