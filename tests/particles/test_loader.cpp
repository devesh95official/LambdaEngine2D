// The .particles.json loader.
#include <string>

#include <lam/assets/asset_server.h>
#include <lam/particles/particles.h>
#include <lam/render/atlas.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>

#include "particles_test_util.h"

using namespace lam;
using lam::test::ParticlesHarness;
using lam::test::TempDir;
using lam::test::write_test_file;

namespace {

Result<AssetId<EmitterConfig>> load(ParticlesHarness &h, const TempDir &dir, std::string_view name,
                                    std::string_view json) {
    write_test_file(dir, name, json);
    return h.assets().load_now<EmitterConfig>(name);
}

} // namespace

TEST_CASE("loader: every key maps to its EmitterConfig field") {
    TempDir dir;
    ParticlesHarness h(AssetsConfig{.root = dir.path()});
    auto id = load(h, dir, "fx/spark.particles.json", R"({
        "capacity": 300, "rate": 40.5, "burst": 12,
        "speed_min": 30, "speed_max": 80, "angle": -1.5, "spread": 0.75,
        "life_min": 0.4, "life_max": 0.9, "size_start": 3, "size_end": 1,
        "color_start": "#ffd080", "color_end": "#ff400000", "gravity": [0, 90], "drag": 1.5,
        "layer": "world", "material": 2, "spawn_area": [-4, -4, 8, 8], "local_space": true,
        "spawn_shape": "ring", "rotation_min": 0.1, "rotation_max": 0.2, "spin_min": -1, "spin_max": 1,
        "align_to_velocity": true, "stretch": 0.02, "sway": 3, "sway_hz": 0.5, "jitter": 2,
        "y_sort": true, "depth": 4, "prewarm": 1.5, "sprite": "square"
    })");
    REQUIRE(id);
    const EmitterConfig *c = h.assets().get(*id);
    REQUIRE(c != nullptr);
    CHECK(c->capacity == 300);
    CHECK(c->rate == 40.5f);
    CHECK(c->burst == 12);
    CHECK(c->speed_min == 30);
    CHECK(c->speed_max == 80);
    CHECK(c->angle == -1.5f);
    CHECK(c->spread == 0.75f);
    CHECK(c->life_min == 0.4f);
    CHECK(c->life_max == 0.9f);
    CHECK(c->size_start == 3);
    CHECK(c->size_end == 1);
    CHECK(c->color_start == Color{255, 208, 128, 255});
    CHECK(c->color_end == Color{255, 64, 0, 0});
    CHECK(c->gravity == Vec2{0, 90});
    CHECK(c->drag == 1.5f);
    CHECK(c->layer == kLayerWorld);
    CHECK(c->material == 2);
    CHECK(c->spawn_area == Rect{-4, -4, 8, 8});
    CHECK(c->local_space);
    CHECK(c->spawn_shape == SpawnShape::Ring);
    CHECK(c->rotation_min == 0.1f);
    CHECK(c->rotation_max == 0.2f);
    CHECK(c->spin_min == -1);
    CHECK(c->spin_max == 1);
    CHECK(c->align_to_velocity);
    CHECK(c->stretch == 0.02f);
    CHECK(c->sway == 3);
    CHECK(c->sway_hz == 0.5f);
    CHECK(c->jitter == 2);
    CHECK(c->y_sort);
    CHECK(c->depth == 4);
    CHECK(c->prewarm == 1.5f);
    CHECK_FALSE(static_cast<bool>(c->region.texture));
    CHECK(c->region.source == kWhiteSource);
}

TEST_CASE("loader: missing keys keep the defaults; layers by number or name") {
    TempDir dir;
    ParticlesHarness h(AssetsConfig{.root = dir.path()});
    auto id = load(h, dir, "a.particles.json", R"({"layer": 200})");
    REQUIRE(id);
    const EmitterConfig *c = h.assets().get(*id);
    REQUIRE(c != nullptr);
    const EmitterConfig d{};
    CHECK(c->capacity == d.capacity);
    CHECK(c->speed_max == d.speed_max);
    CHECK(c->color_end == d.color_end);
    CHECK(c->layer == 200);
    CHECK(c->region.source.empty()); // the default circle
    for (const auto &[name, layer] : {std::pair{"background", kLayerBackground}, std::pair{"screen_fx", kLayerScreenFx},
                                      std::pair{"foreground", kLayerForeground}, std::pair{"ui", kLayerUi}}) {
        CAPTURE(name);
        auto l = load(h, dir, std::string(name) + ".particles.json", std::string(R"({"layer": ")") + name + "\"}");
        REQUIRE(l);
        CHECK(h.assets().get(*l)->layer == layer);
    }
}

TEST_CASE("loader: unknown keys, bad ranges and bad names are errors that name the place") {
    TempDir dir;
    ParticlesHarness h(AssetsConfig{.root = dir.path()});
    struct Bad {
        const char *json;
        const char *needle;
    };
    const Bad cases[] = {
        {R"({"rate": 3, "sped": 4})", "sped"},
        {R"({"capacity": 70000})", "capacity"},
        {R"({"capacity": 0})", "capacity"},
        {R"({"rate": -1})", "rate"},
        {R"({"speed_min": 50, "speed_max": 10})", "speed_max"},
        {R"({"life_min": 0})", "life_min"},
        {R"({"layer": "sky"})", "sky"},
        {R"({"layer": 300})", "layer"},
        {R"({"spawn_shape": "star"})", "star"},
        {R"({"sprite": "heart"})", "heart"},
        {R"({"material": 32})", "material"},
        {R"({"source": [0, 0, 4, 4]})", "source"},
        {R"({"atlas": "fx"})", "atlas"},
        {R"({"sprite": "circle", "image": "x.png"})", "sprite"},
        {R"({"rate": "lots"})", "rate"},
        {R"([1, 2])", "bad"},
        {R"({"rate": 1)", "bad"},
    };
    int n = 0;
    for (const Bad &b : cases) {
        CAPTURE(b.json);
        const std::string name = "bad" + std::to_string(n++) + ".particles.json";
        auto id = load(h, dir, name, b.json);
        REQUIRE_FALSE(id);
        CHECK(id.error().message.find(name) != std::string::npos);
        CHECK(id.error().message.find(b.needle) != std::string::npos);
    }
}

TEST_CASE("loader: an image is packed into the atlas group at finalize") {
    TempDir dir;
    ParticlesHarness h(AssetsConfig{.root = dir.path()});
    if (h.app().try_res<AtlasSet>() == nullptr) h.app().insert_resource<AtlasSet>();
    write_test_file(dir, "fx/sheet.png", lam::test::png_header_bytes(32, 16));
    auto id = load(h, dir, "fx/spark.particles.json",
                   R"({"image": "sheet.png", "source": [8, 0, 8, 8], "atlas": "fx"})");
    REQUIRE(id);
    const EmitterConfig *c = h.assets().get(*id);
    REQUIRE(c != nullptr);
    const auto pages = h.app().res<AtlasSet>().pages(str_id("fx"));
    REQUIRE(pages.size() == 1);
    CHECK(c->region.texture == pages[0]);
    // The image sits after the built-in block; the source is relative to the image.
    CHECK(c->region.source.w == 8);
    CHECK(c->region.source.h == 8);
    CHECK(c->region.size == Vec2{8, 8});
    CHECK((c->region.source.x >= kBuiltinImageSize || c->region.source.y >= kBuiltinImageSize));
    // The whole image when no source is given; the default group.
    auto whole = load(h, dir, "fx/whole.particles.json", R"({"image": "sheet.png"})");
    REQUIRE(whole);
    const EmitterConfig *w = h.assets().get(*whole);
    CHECK(w->region.texture == h.app().res<AtlasSet>().pages(str_id("default"))[0]);
    CHECK(w->region.source.w == 32);
    CHECK(w->region.source.h == 16);
}

TEST_CASE("loader: \"atlas\": \"none\" or no AtlasSet keeps a standalone texture") {
    TempDir dir;
    ParticlesHarness h(AssetsConfig{.root = dir.path()});
    write_test_file(dir, "dot.png", lam::test::png_header_bytes(4, 4));
    SUBCASE("opted out") {
        if (h.app().try_res<AtlasSet>() == nullptr) h.app().insert_resource<AtlasSet>();
        auto id = load(h, dir, "dot.particles.json", R"({"image": "dot.png", "atlas": "none"})");
        REQUIRE(id);
        const EmitterConfig *c = h.assets().get(*id);
        REQUIRE(c != nullptr);
        const Texture *t = h.assets().get(c->region.texture);
        REQUIRE(t != nullptr);
        CHECK(t->width == 4);
        CHECK(c->region.source == Rect{0, 0, 4, 4});
    }
    SUBCASE("no AtlasSet (render not installed)") {
        if (h.app().try_res<AtlasSet>() != nullptr) return; // RenderPlugin provides one
        auto id = load(h, dir, "dot.particles.json", R"({"image": "dot.png", "source": [1, 1, 2, 2]})");
        REQUIRE(id);
        const EmitterConfig *c = h.assets().get(*id);
        REQUIRE(c != nullptr);
        CHECK(h.assets().get(c->region.texture) != nullptr);
        CHECK(c->region.source == Rect{1, 1, 2, 2});
        CHECK(c->region.size == Vec2{2, 2});
    }
}

TEST_CASE("loader: image errors name the config and the image") {
    TempDir dir;
    ParticlesHarness h(AssetsConfig{.root = dir.path()});
    write_test_file(dir, "small.png", lam::test::png_header_bytes(4, 4));
    write_test_file(dir, "junk.png", "not a png at all");
    auto missing = load(h, dir, "m.particles.json", R"({"image": "nothere.png"})");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().message.find("m.particles.json") != std::string::npos);
    CHECK(missing.error().message.find("nothere.png") != std::string::npos);
    auto junk = load(h, dir, "j.particles.json", R"({"image": "junk.png"})");
    REQUIRE_FALSE(junk);
    CHECK(junk.error().message.find("junk.png") != std::string::npos);
    auto outside = load(h, dir, "o.particles.json", R"({"image": "small.png", "source": [2, 2, 4, 4]})");
    REQUIRE_FALSE(outside);
    CHECK(outside.error().message.find("source") != std::string::npos);
    auto empty = load(h, dir, "e.particles.json", R"({"image": "small.png", "source": [0, 0, 0, 4]})");
    REQUIRE_FALSE(empty);
}

TEST_CASE("loader: a loaded config drives an emitter end to end") {
    TempDir dir;
    ParticlesHarness h(AssetsConfig{.root = dir.path()});
    write_test_file(dir, "smoke.particles.json", R"({"capacity": 64, "burst": 9, "life_min": 5, "life_max": 5})");
    const AssetId<EmitterConfig> id = h.assets().load<EmitterConfig>("smoke.particles.json");
    const Entity e = h.emitter_for(id, {3, 4});
    h.frames(2);
    REQUIRE(h.world().has<ParticleRuntime>(e));
    CHECK(h.pools().alive(h.pool_of(e)) == 9);
}

// ---- Regressions --------------------------------------------------------------------------------

TEST_CASE("a standalone emitter image lives and dies with its config (reloads too)") {
    TempDir dir;
    ParticlesHarness h(AssetsConfig{.root = dir.path()});
    write_test_file(dir, "dot.png", lam::test::png_header_bytes(4, 4));
    write_test_file(dir, "dot.particles.json", R"({"image": "dot.png", "atlas": "none"})");
    AssetServer &s = h.assets();
    const AssetScope scene = s.begin_scope();
    auto id = s.load_now<EmitterConfig>("dot.particles.json", scene);
    REQUIRE(id);
    const AssetId<Texture> t1 = s.get(*id)->region.texture;
    REQUIRE(s.get(t1) != nullptr);
    REQUIRE(s.reload(*id).ok());
    const AssetId<Texture> t2 = s.get(*id)->region.texture;
    CHECK_FALSE(t2 == t1);
    s.collect_garbage();
    CHECK(s.state(t1) == LoadState::None);                     // was leaked by every reload
    CHECK(s.get(t2) != nullptr);
    s.end_scope(scene);
    s.collect_garbage();
    h.frames(1);
    CHECK(s.state(*id) == LoadState::None);
    CHECK(s.state(t2) == LoadState::None);                     // was leaked after the config unloaded
}
