// lam_animation_bench: 10k heroes with a 3-clip graph, the motor bridge and a looping Tween, plus
// 10k FrameTweens. Prints best and median ms per App tick and the allocations (should be 0).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <lam/animation/animation_plugin.h>
#include <lam/animation/animator.h>
#include <lam/animation/tween.h>
#include <lam/assets/asset_server.h>
#include <lam/core/alloc.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/components.h>
#include <lam/render/sprite.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>

using namespace lam;

namespace {

constexpr int kHeroes = 10000;
constexpr int kTimedTicks = 300;

double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string png_header(int w, int h) {
    std::string s = "\x89PNG\r\n\x1a\n";
    const auto be32 = [&](std::uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) s.push_back(static_cast<char>((v >> shift) & 0xFF));
    };
    be32(13);
    s += "IHDR";
    be32(static_cast<std::uint32_t>(w));
    be32(static_cast<std::uint32_t>(h));
    s += std::string("\x08\x06\x00\x00\x00", 5);
    be32(0);
    return s;
}

std::string sheet_json() {
    std::string frames;
    for (int i = 0; i < 8; ++i) {
        if (i) frames += ",";
        frames += R"({"filename": "f)" + std::to_string(i) + R"(", "frame": {"x": )" + std::to_string(i * 16) +
                  R"(, "y": 0, "w": 16, "h": 16}, "duration": )" + std::to_string(60 + 20 * (i % 3)) + "}";
    }
    return R"({"frames": [)" + frames +
           R"(], "meta": {"image": "hero.png", "size": {"w": 128, "h": 16}, "frameTags": [
             {"name": "idle", "from": 0, "to": 1}, {"name": "run", "from": 2, "to": 5, "direction": "pingpong"},
             {"name": "attack", "from": 6, "to": 7, "repeat": "1"}]}})";
}

const char *kGraph = R"({"sheet": "hero.json", "start": "idle",
  "clips": [{"name": "idle", "tags": ["idle", "idle"]}, {"name": "run", "tags": ["run", "run"]},
            {"name": "attack", "tags": ["attack", "attack"]}],
  "transitions": [{"from": "idle", "to": "run", "param": "moving", "above": 0.5},
                  {"from": "run", "to": "idle", "param": "moving", "below": 0.5},
                  {"from": "*", "to": "attack", "trigger": "attack"},
                  {"from": "attack", "to": "idle", "on_finish": true}],
  "events": [{"tag": "attack", "frame": 1, "name": "hit"}]})";

// Simulate: random triggers and velocity changes from the world RNG (deterministic).
void drive(Ctx &ctx) {
    Rng &rng = ctx.rng(str_id("bench"));
    ctx.world().view<AnimParams, Velocity>().each([&](Entity, AnimParams &p, Velocity &v) {
        const std::uint32_t r = rng.next_u32();
        if (r % 97 == 0) fire_trigger(p, str_id("attack"));
        if (r % 61 == 0) v.linear.x = static_cast<float>(static_cast<int>(r % 81) - 40);
    });
}

} // namespace

TEST_CASE("bench: 10k animated heroes with graphs, bridge and tweens") {
    test::TempDir dir;
    test::HeadlessApp h(AppConfig{.entity_capacity = 32768});
    h.add<AssetsPlugin>(AssetsConfig{.root = dir.path()});
    h.add<AnimationPlugin>();
    App &app = h.app();
    (void)test::write_test_file(dir, "hero.json", sheet_json());
    (void)test::write_test_file(dir, "hero.png", png_header(128, 16));
    (void)test::write_test_file(dir, "hero.anim.json", kGraph);
    AssetServer &assets = app.res<AssetServer>();
    const Result<AssetId<SpriteSheet>> sheet = assets.load_now<SpriteSheet>("hero.json");
    const Result<AssetId<AnimGraph>> graph = assets.load_now<AnimGraph>("hero.anim.json");
    REQUIRE(sheet.ok());
    REQUIRE(graph.ok());
    app.add_system(Stage::Simulate, &drive, "bench.drive");

    World &w = app.main_world();
    for (int i = 0; i < kHeroes; ++i) {
        const Entity e = w.create();
        w.add<Transform>(e, Transform{.position = {static_cast<float>(i % 100) * 8, static_cast<float>(i / 100) * 8}});
        w.add<Sprite>(e);
        w.add<Animator>(e, Animator{.sheet = *sheet, .graph = *graph, .speed = 0.75f + 0.125f * float(i % 5)});
        w.add<AnimParams>(e);
        w.add<MotorAnimBridge>(e);
        w.add<Velocity>(e);
        TweenSpec bob;
        bob.to = {0, -3};
        bob.ticks = static_cast<std::uint16_t>(12 + i % 9);
        bob.repeat = 255;
        bob.yoyo = true;
        bob.relative = true;
        tween_to(w.commands(), e, bob);
        TweenSpec squash;
        squash.target = TweenTarget::Scale;
        squash.from = {1, 1};
        squash.to = {1.15f, 0.9f};
        squash.ticks = 10;
        squash.repeat = 255;
        squash.yoyo = true;
        frame_tween_to(w.commands(), e, squash);
    }
    w.commands().apply();
    test::run_ticks(app, 120); // warm-up: channels, pools and caches reach their steady size

    std::vector<double> ticks;
    ticks.reserve(kTimedTicks);
    const std::uint64_t allocs_before = allocation_count();
    for (int i = 0; i < kTimedTicks; ++i) {
        const double t0 = now_ms();
        test::run_ticks(app, 1);
        ticks.push_back(now_ms() - t0);
    }
    const std::uint64_t allocs = allocation_count() - allocs_before;
    std::sort(ticks.begin(), ticks.end());
    std::printf("lam_animation_bench: %d heroes, %d ticks: best %.3f ms, median %.3f ms per tick "
                "(app tick incl. frame stages); allocations: %llu%s\n",
                kHeroes, kTimedTicks, ticks.front(), ticks[ticks.size() / 2],
                static_cast<unsigned long long>(allocs),
                allocation_counting_enabled() ? "" : " (counting off)");
    if (allocation_counting_enabled()) CHECK(allocs == 0);
}
