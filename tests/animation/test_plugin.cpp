// AnimationPlugin inside an App: loaders, prefabs, the motor bridge, events and determinism.
#include <cstdint>
#include <string>
#include <vector>

#include <lam/animation/animation_plugin.h>
#include <lam/animation/animator.h>
#include <lam/animation/tween.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/physics/components.h>
#include <lam/physics/motors.h>
#include <lam/render/atlas.h>
#include <lam/render/sprite.h>

#include "anim_test_util.h"

using anim_test::AnimApp;
using anim_test::contains;
using anim_test::png_header;
using lam::str_id;

namespace {

// An Aseprite-shaped sheet: `count` 16x16 frames in a row (100 ms), the given tags JSON.
std::string sheet_json(int count, const std::string &tags, const std::string &extra = "",
                       const std::string &image = "hero.png") {
    std::string frames;
    for (int i = 0; i < count; ++i) {
        if (i) frames += ",";
        frames += R"({"filename": "f)" + std::to_string(i) + R"(", "frame": {"x": )" +
                  std::to_string(i * 16) + R"(, "y": 0, "w": 16, "h": 16}, "duration": 100})";
    }
    return R"({"frames": [)" + frames + R"(], "meta": {"app": "test", "image": ")" + image +
           R"(", "size": {"w": )" + std::to_string(count * 16) + R"(, "h": 16}, "frameTags": )" +
           tags + extra + "}}";
}

const char *kHeroTags = R"([{"name": "idle", "from": 0, "to": 1},
    {"name": "run", "from": 2, "to": 5},
    {"name": "attack", "from": 6, "to": 7, "repeat": "1"},
    {"name": "walk_side", "from": 8, "to": 9}, {"name": "walk_down", "from": 10, "to": 11},
    {"name": "walk_up", "from": 12, "to": 13}])";

const char *kHeroGraph = R"({"sheet": "hero.json", "start": "idle",
  "clips": [{"name": "idle", "tags": ["idle", "idle"]}, {"name": "run", "tags": ["run", "run"]},
            {"name": "attack", "tags": ["attack", "attack"]}],
  "transitions": [
    {"from": "idle", "to": "run", "param": "moving", "above": 0.5},
    {"from": "run", "to": "idle", "param": "moving", "below": 0.5},
    {"from": "*", "to": "attack", "trigger": "attack"},
    {"from": "attack", "to": "idle", "on_finish": true}],
  "events": [{"tag": "attack", "frame": 1, "name": "hit"}]})";

const char *kWalkGraph = R"({"sheet": "hero.json",
  "clips": [{"name": "walk", "tags": ["walk_side", "walk_down", "", "walk_up"]}]})";

void write_hero(AnimApp &a) {
    a.write("hero.json", sheet_json(14, kHeroTags));
    a.write("hero.png", png_header(14 * 16, 16));
    a.write("hero.anim.json", kHeroGraph);
    a.write("walk.anim.json", kWalkGraph);
}

lam::AtlasSet &ensure_atlas(lam::App &app) {
    if (lam::AtlasSet *a = app.try_res<lam::AtlasSet>()) return *a;
    return app.insert_resource<lam::AtlasSet>();
}

template <class T> lam::AssetId<T> load_ok(AnimApp &a, std::string_view path) {
    lam::Result<lam::AssetId<T>> r = a.assets().load_now<T>(path);
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return r.value();
}

std::string load_error(AnimApp &a, std::string_view path) {
    lam::test::CaptureLog quiet; // the asset server logs failures
    lam::Result<lam::AssetId<lam::SpriteSheet>> r = a.assets().load_now<lam::SpriteSheet>(path);
    CHECK_FALSE(r.ok());
    return r.ok() ? std::string() : r.error().message;
}

// A hero entity: Transform, Sprite, Animator (sheet + graph), AnimParams.
lam::Entity spawn_hero(lam::World &w, lam::AssetId<lam::SpriteSheet> sheet,
                       lam::AssetId<lam::AnimGraph> graph, lam::Vec2 at = {}) {
    const lam::Entity e = w.create();
    w.add<lam::Transform>(e, lam::Transform{.position = at});
    w.add<lam::Sprite>(e);
    w.add<lam::Animator>(e, lam::Animator{.sheet = sheet, .graph = graph});
    w.add<lam::AnimParams>(e);
    return e;
}

} // namespace

TEST_CASE("animation plugin: systems run at their stages and orders") {
    AnimApp a;
    const std::string s = a.app().schedule_text();
    CHECK(contains(s, "-1500  motor_anim_bridge  [lam.animation]"));
    CHECK(contains(s, "-1400  animators  [lam.animation]"));
    CHECK(contains(s, "-1300  tweens  [lam.animation]"));
    CHECK(contains(s, "-1500  frame_tweens  [lam.animation]"));
    CHECK(s.find("motor_anim_bridge") < s.find("animators"));
    CHECK(s.find("animators") < s.find("tweens"));
    CHECK(s.find("tweens") < s.find("frame_tweens"));
    CHECK(a.world().events().try_get<lam::AnimationEvent>() != nullptr);
    CHECK(a.world().events().try_get<lam::TweenFinished>() != nullptr);
    CHECK(lam::ComponentRegistry::global().find("Animator") != nullptr);
    CHECK(lam::ComponentRegistry::global().find("FrameTween") != nullptr);
    const lam::ComponentInfo *ft = lam::ComponentRegistry::global().find("FrameTween");
    REQUIRE(ft != nullptr);
    CHECK((ft->flags & lam::kHash) == 0);
    CHECK((lam::ComponentRegistry::global().find("Tween")->flags & lam::kHash) != 0);
}

TEST_CASE("sheet loader: an Aseprite sheet packs into the default atlas group") {
    AnimApp a;
    lam::AtlasSet &atlas = ensure_atlas(a.app());
    write_hero(a);
    const auto id = load_ok<lam::SpriteSheet>(a, "hero.json");
    const lam::SpriteSheet *s = a.assets().get(id);
    REQUIRE(s != nullptr);
    REQUIRE(atlas.pages(str_id("default")).size() == 1);
    CHECK(s->texture == atlas.pages(str_id("default"))[0]);
    REQUIRE(s->regions.size() == 14);
    const lam::Vec2 base = s->regions[0].source.min() - s->frames[0].source.min();
    CHECK(base.x >= lam::kBuiltinImageSize); // beside the built-in block
    for (std::size_t i = 0; i < s->frames.size(); ++i) {
        CHECK(s->regions[i].texture == s->texture);
        CHECK(s->regions[i].source == s->frames[i].source.translated(base));
        CHECK(s->regions[i].size == lam::Vec2{16, 16});
    }
    CHECK_FALSE(s->pending_image.has_pixels());
    CHECK(s->pending_image.width == 0);
    // Loading again is the same asset.
    CHECK(a.assets().load<lam::SpriteSheet>("hero.json") == id);

    SUBCASE("a named atlas group gets its own pages") {
        a.write("ui.json", sheet_json(2, "[]", R"(, "atlas": "ui")", "ui.png"));
        a.write("ui.png", png_header(32, 16));
        const lam::SpriteSheet *ui = a.assets().get(load_ok<lam::SpriteSheet>(a, "ui.json"));
        REQUIRE(ui != nullptr);
        REQUIRE(atlas.pages(str_id("ui")).size() == 1);
        CHECK(ui->texture == atlas.pages(str_id("ui"))[0]);
    }
    SUBCASE("\"atlas\": \"none\" keeps a standalone texture") {
        a.write("solo.json", sheet_json(2, "[]", R"(, "atlas": "none")", "solo.png"));
        a.write("solo.png", png_header(32, 16));
        const lam::SpriteSheet *solo = a.assets().get(load_ok<lam::SpriteSheet>(a, "solo.json"));
        REQUIRE(solo != nullptr);
        CHECK(solo->texture != atlas.pages(str_id("default"))[0]);
        const lam::Texture *t = a.assets().get(solo->texture);
        REQUIRE(t != nullptr);
        CHECK(t->width == 32);
        CHECK(solo->regions[1].source == lam::Rect{16, 0, 16, 16});
    }
}

TEST_CASE("sheet loader: without an AtlasSet the sheet keeps its own texture") {
    AnimApp a;
    write_hero(a);
    const lam::SpriteSheet *s = a.assets().get(load_ok<lam::SpriteSheet>(a, "hero.json"));
    REQUIRE(s != nullptr);
    const lam::Texture *t = a.assets().get(s->texture);
    REQUIRE(t != nullptr);
    CHECK(t->width == 224);
    CHECK(t->height == 16);
    CHECK(s->regions[3].source == lam::Rect{48, 0, 16, 16});
}

TEST_CASE("sheet loader: a bare .png is a one-frame sheet") {
    AnimApp a;
    ensure_atlas(a.app());
    a.write("coin.png", png_header(12, 20));
    const lam::SpriteSheet *s = a.assets().get(load_ok<lam::SpriteSheet>(a, "coin.png"));
    REQUIRE(s != nullptr);
    REQUIRE(s->frames.size() == 1);
    CHECK(s->frames[0].source_size == lam::Vec2{12, 20});
    CHECK(s->regions[0].size == lam::Vec2{12, 20});
    CHECK(s->image_path == "coin.png");
}

TEST_CASE("sheet loader: errors name the file once and say what is wrong") {
    AnimApp a;
    a.write("bad.json", R"({"frames": [], "meta": {"image": "x.png", "size": {"w": 8, "h": 8}}})");
    const std::string bad = load_error(a, "bad.json");
    CHECK(contains(bad, "bad.json: /frames: must not be empty"));
    CHECK_FALSE(contains(bad, "bad.json: bad.json"));
    a.write("noimg.json", sheet_json(2, "[]", "", "missing.png"));
    CHECK(contains(load_error(a, "noimg.json"), "missing.png"));
    a.write("size.json", sheet_json(2, "[]", "", "size.png"));
    a.write("size.png", png_header(33, 16));
    CHECK(contains(load_error(a, "size.json"),
                   "image 'size.png' is 33 x 16 but meta.size says 32 x 16"));
    a.write("notpng.json", sheet_json(2, "[]", "", "notpng.png"));
    a.write("notpng.png", "GIF89a");
    CHECK(contains(load_error(a, "notpng.json"), "image 'notpng.png'"));
}

TEST_CASE("graph loader: the sheet is a dependency and every tag is checked") {
    AnimApp a;
    write_hero(a);
    const auto gid = load_ok<lam::AnimGraph>(a, "hero.anim.json");
    const lam::AnimGraph *g = a.assets().get(gid);
    REQUIRE(g != nullptr);
    CHECK(g->sheet == a.assets().load<lam::SpriteSheet>("hero.json"));
    CHECK(a.assets().state(g->sheet) == lam::LoadState::Ready);
    CHECK(g->clips.size() == 3);

    a.write("broken.anim.json", R"({"sheet": "hero.json", "clips": [{"name": "swim"}]})");
    {
        lam::test::CaptureLog quiet;
        lam::Result<lam::AssetId<lam::AnimGraph>> r = a.assets().load_now<lam::AnimGraph>("broken.anim.json");
        REQUIRE_FALSE(r.ok());
        CHECK(contains(r.error().message, "clip \"swim\": tag \"swim\" is not in the sheet"));
    }
    a.write("nosheet.anim.json", R"({"sheet": "nope.json", "clips": [{"name": "idle"}]})");
    {
        lam::test::CaptureLog quiet;
        CHECK_FALSE(a.assets().load_now<lam::AnimGraph>("nosheet.anim.json").ok());
    }
    a.write("free.anim.json", R"({"clips": [{"name": "idle"}]})");
    const lam::AnimGraph *free = a.assets().get(load_ok<lam::AnimGraph>(a, "free.anim.json"));
    REQUIRE(free != nullptr);
    CHECK_FALSE(free->sheet);
}

TEST_CASE("animators: prefab-spawned heroes play their graph and write their sprites") {
    AnimApp a;
    ensure_atlas(a.app());
    write_hero(a);
    lam::PrefabLibrary &prefabs = a.app().res<lam::PrefabLibrary>();
    const lam::Status st = prefabs.load_text(R"({"prefabs": {"hero": {"components": {
        "Transform": {}, "Sprite": {"layer": 64},
        "Animator": {"sheet": "hero.json", "graph": "hero.anim.json", "clip": "idle"},
        "AnimParams": {}, "MotorAnimBridge": {}, "Velocity": {}}}}})",
                                             "prefabs.json", a.app().serialize_context());
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    lam::World &w = a.world();
    const lam::Entity hero = prefabs.spawn(w, str_id("hero"), {32, 32});
    const lam::Animator &an = w.get<lam::Animator>(hero);
    CHECK(an.sheet);
    CHECK(an.graph);
    a.ticks(1);
    const lam::SpriteSheet *s = a.assets().get(an.sheet);
    REQUIRE(s != nullptr);
    CHECK(w.get<lam::Animator>(hero).tag == 0);
    CHECK(w.get<lam::Sprite>(hero).region.source == s->regions[0].source);
    // The bridge sees the velocity: moving -> run on the same tick; facing left mirrors.
    w.get<lam::Velocity>(hero).linear = {-40, 0};
    a.ticks(1);
    CHECK(w.get<lam::Animator>(hero).clip == str_id("run"));
    CHECK(w.get<lam::Animator>(hero).facing == 1);
    CHECK((w.get<lam::Sprite>(hero).flags & lam::kFlipX) != 0);
    CHECK(w.get<lam::Sprite>(hero).region.source == s->regions[2].source);
    CHECK(lam::get_param(w.get<lam::AnimParams>(hero), str_id("speed")) == 40.0f);
}

TEST_CASE("motor bridge: platformer, top-down and velocity-only bodies") {
    AnimApp a;
    write_hero(a);
    const auto sheet = load_ok<lam::SpriteSheet>(a, "hero.json");
    const auto walk = load_ok<lam::AnimGraph>(a, "walk.anim.json");
    lam::World &w = a.world();

    const lam::Entity plat = spawn_hero(w, sheet, {});
    w.add<lam::MotorAnimBridge>(plat);
    w.add<lam::PlatformerMotor>(plat, lam::PlatformerMotor{.move_x = -1, .on_ground = false,
                                                           .on_ladder = true});
    w.add<lam::Velocity>(plat, lam::Velocity{{-30, 45}});

    const lam::Entity top = spawn_hero(w, sheet, walk);
    w.add<lam::MotorAnimBridge>(top);
    w.add<lam::TopDownMotor>(top, lam::TopDownMotor{.move = {0, -1}});
    w.add<lam::Velocity>(top, lam::Velocity{{3, -4}});

    const lam::Entity bare = w.create();
    w.add<lam::Animator>(bare, lam::Animator{.sheet = sheet});
    w.add<lam::MotorAnimBridge>(bare);
    w.add<lam::Velocity>(bare, lam::Velocity{{-0.5f, 0}}); // too slow to turn

    const lam::Entity full = spawn_hero(w, sheet, {});
    w.add<lam::MotorAnimBridge>(full);
    lam::AnimParams &fp = w.get<lam::AnimParams>(full);
    for (int i = 0; i < 7; ++i) lam::set_param(fp, lam::intern("game" + std::to_string(i)), 1.0f);

    a.ticks(1);
    const lam::AnimParams &pp = w.get<lam::AnimParams>(plat);
    CHECK(lam::get_param(pp, str_id("speed")) == 30.0f); // |vx| for platformers
    CHECK(lam::get_param(pp, str_id("vx")) == -30.0f);
    CHECK(lam::get_param(pp, str_id("vy")) == 45.0f);
    CHECK(lam::get_param(pp, str_id("grounded")) == 0.0f);
    CHECK(lam::get_param(pp, str_id("moving")) == 1.0f);
    CHECK(lam::get_param(pp, str_id("climbing")) == 1.0f);
    CHECK(w.get<lam::Animator>(plat).facing == 1); // intent left

    const lam::AnimParams &tp = w.get<lam::AnimParams>(top);
    CHECK(lam::get_param(tp, str_id("speed")) == 5.0f);
    CHECK(lam::get_param(tp, str_id("grounded")) == 1.0f);
    CHECK(w.get<lam::Animator>(top).facing == 3); // 4-direction graph: up
    CHECK(w.get<lam::Animator>(top).frame == 12);  // walk_up

    CHECK(w.get<lam::Animator>(bare).facing == 0);
    w.get<lam::Velocity>(bare).linear = {-20, 5};
    a.ticks(1);
    CHECK(w.get<lam::Animator>(bare).facing == 1);

    // A full param set only gets the bridge params that fit (one slot left: speed).
    const lam::AnimParams &full_p = w.get<lam::AnimParams>(full);
    CHECK(full_p.values[7].name == str_id("speed"));
    CHECK(lam::get_param(full_p, str_id("vx")) == 0.0f);
    CHECK(lam::get_param(full_p, str_id("game6")) == 1.0f);

    // Releasing the intent keeps the facing.
    w.get<lam::PlatformerMotor>(plat).move_x = 0;
    w.get<lam::TopDownMotor>(top).move = {};
    a.ticks(1);
    CHECK(w.get<lam::Animator>(plat).facing == 1);
    CHECK(w.get<lam::Animator>(top).facing == 3);
}

TEST_CASE("animation events are read by Simulate on the next tick") {
    AnimApp a;
    anim_test::EventLog &log = anim_test::log_events(a.app());
    write_hero(a);
    lam::World &w = a.world();
    const lam::Entity e = spawn_hero(w, load_ok<lam::SpriteSheet>(a, "hero.json"),
                                     load_ok<lam::AnimGraph>(a, "hero.anim.json"));
    a.ticks(2);
    lam::fire_trigger(w.get<lam::AnimParams>(e), str_id("attack"));
    std::uint64_t entered = 0;
    for (int i = 0; i < 20 && entered == 0; ++i) {
        a.ticks(1);
        if (w.get<lam::Animator>(e).frame == 7) entered = w.tick();
    }
    REQUIRE(entered != 0);
    CHECK(log.count_anim(str_id("hit")) == 0);
    a.ticks(1);
    REQUIRE(log.count_anim(str_id("hit")) == 1);
    CHECK(log.items.back().tick == entered + 1);
    CHECK(log.items.back().anim.entity == e);
    CHECK(log.items.back().anim.frame == 1);
    a.ticks(20);
    CHECK(log.count_anim(str_id("hit")) == 1); // attack plays once
    CHECK(w.get<lam::Animator>(e).clip == str_id("idle"));
}

TEST_CASE("animators: a graph that failed to load, or a missing sheet, is skipped safely") {
    AnimApp a;
    lam::World &w = a.world();
    const lam::Entity e = w.create();
    w.add<lam::Animator>(e, lam::Animator{.sheet = lam::AssetId<lam::SpriteSheet>{42, 1},
                                          .graph = lam::AssetId<lam::AnimGraph>{9, 3}});
    w.add<lam::Sprite>(e);
    const lam::Entity f = w.create();
    w.add<lam::Animator>(f);
    a.ticks(5);
    CHECK(w.get<lam::Animator>(e).tag == -1);
    CHECK(w.get<lam::Animator>(f).frame == 0);
}

TEST_CASE("animators: a hot-reloaded sheet re-resolves tags by name") {
    AnimApp a;
    a.write("s.json", sheet_json(4, R"([{"name": "a", "from": 0, "to": 1}, {"name": "b", "from": 2, "to": 3}])",
                                 "", "s.png"));
    a.write("s.png", png_header(64, 16));
    const auto sid = load_ok<lam::SpriteSheet>(a, "s.json");
    lam::World &w = a.world();
    const lam::Entity e = w.create();
    w.add<lam::Animator>(e, lam::Animator{.sheet = sid, .clip = str_id("b")});
    a.ticks(3);
    CHECK(w.get<lam::Animator>(e).tag == 1);
    CHECK(w.get<lam::Animator>(e).frame == 2);
    // Same frames, tags listed the other way round.
    a.write("s.json", sheet_json(4, R"([{"name": "b", "from": 2, "to": 3}, {"name": "a", "from": 0, "to": 1}])",
                                 "", "s.png"));
    REQUIRE(a.assets().reload(sid).ok());
    a.ticks(1);
    CHECK(w.get<lam::Animator>(e).tag == 0);
    CHECK(w.get<lam::Animator>(e).frame == 2); // restarted on "b"
}

TEST_CASE("animation systems run in every world of the stack") {
    AnimApp a;
    write_hero(a);
    const auto sheet = load_ok<lam::SpriteSheet>(a, "hero.json");
    lam::World &other = a.app().create_world("other");
    const lam::Entity e = other.create();
    other.add<lam::Animator>(e, lam::Animator{.sheet = sheet, .clip = str_id("run")});
    const lam::WorldEntry stack[] = {{&a.world()}, {&other}};
    a.app().set_world_stack(stack);
    a.ticks(1 + 6);
    CHECK(other.get<lam::Animator>(e).frame == 3);
}

namespace {

// A busy scene: heroes with graphs, params, triggers and the bridge; looping tweens and frame
// tweens. `driver` fires triggers and moves bodies from the world RNG (deterministic).
void drive(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    lam::Rng &rng = ctx.rng(str_id("anim_test"));
    w.view<lam::AnimParams, lam::Velocity>().each([&](lam::Entity, lam::AnimParams &p, lam::Velocity &v) {
        if (rng.next_u32() % 23 == 0) lam::fire_trigger(p, str_id("attack"));
        if (rng.next_u32() % 17 == 0) v.linear.x = static_cast<float>(static_cast<int>(rng.next_u32() % 81) - 40);
    });
}

void build_scene(AnimApp &a, int heroes) {
    write_hero(a);
    a.app().add_system(lam::Stage::Simulate, &drive, "test.drive");
    const auto sheet = load_ok<lam::SpriteSheet>(a, "hero.json");
    const auto graph = load_ok<lam::AnimGraph>(a, "hero.anim.json");
    lam::World &w = a.world();
    for (int i = 0; i < heroes; ++i) {
        const lam::Entity e = spawn_hero(w, sheet, graph, {static_cast<float>(i * 8), 0});
        w.add<lam::MotorAnimBridge>(e);
        w.add<lam::Velocity>(e);
        w.get<lam::Animator>(e).speed = 0.5f + static_cast<float>(i % 5) * 0.25f;
        lam::TweenSpec bob;
        bob.target = lam::TweenTarget::Position;
        bob.to = {0, -4};
        bob.ticks = static_cast<std::uint16_t>(10 + i % 7);
        bob.repeat = 255;
        bob.yoyo = true;
        bob.relative = true;
        lam::tween_to(w.commands(), e, bob);
        lam::TweenSpec squash;
        squash.target = lam::TweenTarget::Scale;
        squash.from = {1, 1};
        squash.to = {1.2f, 0.8f};
        squash.ticks = 9;
        squash.repeat = 255;
        squash.yoyo = true;
        lam::frame_tween_to(w.commands(), e, squash);
    }
    w.commands().apply();
}

} // namespace

TEST_CASE("determinism: identical runs hash identically; a restored snapshot continues exactly") {
    AnimApp a;
    AnimApp b;
    build_scene(a, 40);
    build_scene(b, 40);
    std::uint64_t prev = 0;
    bool changed = true;
    for (int t = 0; t < 240; ++t) {
        a.ticks(1);
        b.ticks(1);
        const std::uint64_t h = a.world().state_hash();
        if (h != b.world().state_hash()) {
            FAIL("hash differs at tick " << t);
            break;
        }
        changed = changed && h != prev;
        prev = h;
    }
    CHECK(changed);

    lam::WorldSnapshot snap;
    a.world().snapshot_into(snap);
    a.ticks(120);
    const std::uint64_t later = a.world().state_hash();
    lam::Animator sample = a.world().get<lam::Animator>(lam::Entity{5, 1});
    a.world().restore(snap);
    a.ticks(120);
    CHECK(a.world().state_hash() == later);
    const lam::Animator again = a.world().get<lam::Animator>(lam::Entity{5, 1});
    CHECK(again.frame == sample.frame);
    CHECK(again.elapsed_milliticks == sample.elapsed_milliticks);
    CHECK(again.clip == sample.clip);
}

TEST_CASE("animation: no allocation per tick or frame in steady state") {
    AnimApp a;
    build_scene(a, 200);
    lam::test::run_ticks(a.app(), 120);
    CHECK_NO_ALLOC(lam::test::run_ticks(a.app(), 600));
    CHECK_NO_ALLOC(lam::test::run_frames(a.app(), 60));
}

TEST_CASE("animation plugin: added before AssetsPlugin, it registers its loaders at finish") {
    lam::test::TempDir dir;
    lam::test::HeadlessApp h;
    h.add<lam::AnimationPlugin>();
    h.add<lam::AssetsPlugin>(lam::AssetsConfig{.root = dir.path()});
    (void)lam::test::write_test_file(dir, "coin.png", png_header(8, 8));
    {
        lam::test::CaptureLog log;
        h.frames(1); // finish() runs before the first frame
        CHECK(log.contains("AssetsPlugin was added after AnimationPlugin"));
    }
    lam::Result<lam::AssetId<lam::SpriteSheet>> r = h.app().res<lam::AssetServer>().load_now<lam::SpriteSheet>("coin.png");
    REQUIRE(r.ok());
    CHECK(h.app().res<lam::AssetServer>().get(*r)->frames.size() == 1);
}

namespace {

std::string write_component(const char *type, const void *value, const lam::SerializeContext &ctx) {
    const lam::ComponentInfo *info = lam::ComponentRegistry::global().find(type);
    REQUIRE(info != nullptr);
    lam::DataWriter w;
    w.begin_object().key("v");
    info->write(value, w, ctx);
    w.end_object();
    lam::Result<std::string> s = w.finish();
    REQUIRE(s.ok());
    return *s;
}

void read_component(const char *type, void *value, std::string_view text, const lam::SerializeContext &ctx) {
    const lam::ComponentInfo *info = lam::ComponentRegistry::global().find(type);
    REQUIRE(info != nullptr);
    lam::Result<lam::DataDocument> doc = lam::DataDocument::parse(text, "save.json");
    REQUIRE(doc.ok());
    lam::DataReader root = doc->root();
    lam::DataReader v = root.object("v");
    const lam::Status st = info->read(value, v, ctx);
    CHECK_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    const lam::Status fin = doc->finish();
    CHECK_MESSAGE(fin.ok(), (fin.ok() ? std::string() : fin.error().message));
}

} // namespace

TEST_CASE("saves: Animator, AnimParams and Tween round-trip with names and asset paths") {
    AnimApp a;
    write_hero(a);
    lam::World &w = a.world();
    const lam::Entity e = spawn_hero(w, load_ok<lam::SpriteSheet>(a, "hero.json"),
                                     load_ok<lam::AnimGraph>(a, "hero.anim.json"));
    w.add<lam::MotorAnimBridge>(e);
    w.add<lam::Velocity>(e, lam::Velocity{{-25, 0}});
    lam::TweenSpec bob;
    bob.to = {0, -3};
    bob.repeat = 255;
    bob.yoyo = true;
    bob.relative = true;
    bob.tag = lam::intern("bob");
    lam::tween_to(w.commands(), e, bob);
    w.commands().apply();
    a.ticks(9);
    const lam::SerializeContext ctx = a.app().serialize_context();

    const lam::Animator &an = w.get<lam::Animator>(e);
    const std::string text = write_component("Animator", &an, ctx);
    CHECK(contains(text, "\"hero.json\""));
    CHECK(contains(text, "\"hero.anim.json\""));
    CHECK(contains(text, "\"run\""));
    lam::Animator back;
    read_component("Animator", &back, text, ctx);
    CHECK(back.sheet == an.sheet);
    CHECK(back.graph == an.graph);
    CHECK(back.clip == an.clip);
    CHECK(back.tag == an.tag);
    CHECK(back.frame == an.frame);
    CHECK(back.elapsed_milliticks == an.elapsed_milliticks);
    CHECK(back.facing == an.facing);
    CHECK(back.ping_dir == an.ping_dir);

    const lam::AnimParams &p = w.get<lam::AnimParams>(e);
    const std::string ptext = write_component("AnimParams", &p, ctx);
    for (const char *name : {"\"speed\"", "\"vx\"", "\"vy\"", "\"grounded\"", "\"moving\"", "\"climbing\""})
        CHECK(contains(ptext, name)); // bridge params save as names, not "#hash"
    lam::AnimParams pback;
    read_component("AnimParams", &pback, ptext, ctx);
    for (std::size_t i = 0; i < p.values.size(); ++i) {
        CHECK(pback.values[i].name == p.values[i].name);
        CHECK(pback.values[i].value == p.values[i].value);
    }

    const lam::Tween &t = w.get<lam::Tween>(e);
    const std::string ttext = write_component("Tween", &t, ctx);
    CHECK(contains(ttext, "\"bob\""));
    lam::Tween tback;
    read_component("Tween", &tback, ttext, ctx);
    CHECK(tback.count == t.count);
    CHECK(tback.elapsed == t.elapsed);
    CHECK(tback.steps[0].to == t.steps[0].to); // absolute after the relative start
    CHECK_FALSE(tback.steps[0].relative);
    CHECK(tback.steps[0].tag == t.steps[0].tag);
}

TEST_CASE("canaries: the determinism and allocation checks can fail") {
    // A one-millitick difference in one animator's speed must change the hash (Animator is hashed).
    AnimApp a;
    AnimApp b;
    build_scene(a, 8);
    build_scene(b, 8);
    b.world().get<lam::Animator>(lam::Entity{3, 1}).speed += 0.001f;
    a.ticks(120);
    b.ticks(120);
    MUST_FAIL(CHECK(a.world().state_hash() == b.world().state_hash()));
    // Loading a sheet allocates, so CHECK_NO_ALLOC must catch it (when counting is on).
    if (lam::allocation_counting_enabled()) {
        a.write("late.json", sheet_json(2, "[]", "", "late.png"));
        a.write("late.png", png_header(32, 16));
        MUST_FAIL(CHECK_NO_ALLOC((void)a.assets().load_now<lam::SpriteSheet>("late.json")));
    }
}

// ---- Regressions --------------------------------------------------------------------------------

TEST_CASE("a graph of 1-tag clips flips its sprite when facing left") {
    AnimApp a;
    write_hero(a);
    a.write("side.anim.json", R"({"sheet": "hero.json", "clips": [{"name": "idle"}, {"name": "run"}]})");
    const auto sheet = load_ok<lam::SpriteSheet>(a, "hero.json");
    const auto graph = load_ok<lam::AnimGraph>(a, "side.anim.json");
    CHECK(a.assets().get(graph)->facing_dirs() == 2);           // was 1: no facing pointed left
    lam::World &w = a.world();
    const lam::Entity hero = spawn_hero(w, sheet, graph);
    w.add<lam::MotorAnimBridge>(hero);
    w.add<lam::PlatformerMotor>(hero, lam::PlatformerMotor{.move_x = -1});
    w.add<lam::Velocity>(hero, lam::Velocity{{-40, 0}});
    a.ticks(2);
    CHECK(w.get<lam::Animator>(hero).facing == 1);
    CHECK((w.get<lam::Sprite>(hero).flags & lam::kFlipX) != 0); // runs left drawn facing left
    w.get<lam::PlatformerMotor>(hero).move_x = 1;
    w.get<lam::Velocity>(hero).linear = {40, 0};
    a.ticks(1);
    CHECK(w.get<lam::Animator>(hero).facing == 0);
    CHECK((w.get<lam::Sprite>(hero).flags & lam::kFlipX) == 0);
}

TEST_CASE("a standalone sheet texture lives and dies with its sheet") {
    AnimApp a; // synchronous assets: finalize runs inside load(), in whatever scope is current
    a.write("solo.json", sheet_json(2, "[]", R"(, "atlas": "none")", "solo.png"));
    a.write("solo.png", png_header(32, 16));
    lam::AssetServer &s = a.assets();
    const lam::AssetScope scene_a = s.begin_scope();
    s.set_current_scope(scene_a);
    const auto sheet = load_ok<lam::SpriteSheet>(a, "solo.json");
    const lam::AssetScope scene_b = s.begin_scope();             // scene B replaces A ...
    s.set_current_scope(scene_b);
    CHECK(s.load<lam::SpriteSheet>("solo.json") == sheet);       // ... and loads the same sheet
    s.end_scope(scene_a);
    s.collect_garbage();
    REQUIRE(s.get(sheet) != nullptr);
    const lam::AssetId<lam::Texture> tex = s.get(sheet)->texture;
    CHECK(s.get(tex) != nullptr);                                // was unloaded with scene A
    s.set_current_scope(lam::kRootScope);
    s.end_scope(scene_b);
    s.collect_garbage();
    CHECK(s.state(sheet) == lam::LoadState::None);
    CHECK(s.state(tex) == lam::LoadState::None);                 // not leaked either
}
