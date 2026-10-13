// ParticlesPlugin and its systems.
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/log.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/particles/components.h>
#include <lam/particles/particles.h>
#include <lam/render/render_queue.h>
#include <lam/test/support.h>

#include "particles_test_util.h"

using namespace lam;
using lam::test::ParticlesHarness;
using lam::test::steady_config;

namespace {

// Test resource: what the Simulate system below emits each tick.
struct BurstPlan {
    AssetId<EmitterConfig> config;
    std::uint16_t count = 0;
    Vec2 position;
    int every = 1;
    int ticks = 0;
};

void emit_bursts(Ctx &ctx) {
    BurstPlan *plan = ctx.try_res<BurstPlan>();
    if (plan == nullptr || !plan->config) return;
    if (plan->ticks++ % plan->every != 0) return;
    ctx.emit(BurstAt{plan->config, plan->position, plan->count});
}

void add_burst_system(App &app, BurstPlan plan) {
    app.insert_resource<BurstPlan>(plan);
    app.add_system(Stage::Simulate, &emit_bursts, "test.emit_bursts");
}

std::string schedule_line(const App &app, std::string_view system) {
    const std::string text = app.schedule_text();
    std::size_t at = text.find(std::string(system) + "  [");
    if (at == std::string::npos) return {};
    const std::size_t begin = text.rfind('\n', at);
    const std::size_t end = text.find('\n', at);
    return text.substr(begin == std::string::npos ? 0 : begin + 1,
                       (end == std::string::npos ? text.size() : end) - (begin == std::string::npos ? 0 : begin + 1));
}

} // namespace

TEST_CASE("plugin: registers its systems at their stage, scope and order") {
    ParticlesHarness h;
    const std::string update = schedule_line(h.app(), "particles_update");
    const std::string draw = schedule_line(h.app(), "draw_particles");
    CHECK(update == std::format("{:<11} {:<8} {:>6}  {}  [lam.particles] run_if", "Update", "PerWorld", -1000,
                                "particles_update"));
    CHECK(draw == std::format("{:<11} {:<8} {:>6}  {}  [lam.particles] run_if", "Render", "PerWorld", -800,
                              "draw_particles"));
    CHECK(h.world().resources().has<ParticlePools>());
    CHECK(h.app().events().try_get<AssetEvent<EmitterConfig>>() != nullptr);
    CHECK(h.world().events().try_get<BurstAt>() != nullptr);
    // Every later world gets its own pools.
    World &other = h.app().create_world("other");
    CHECK(other.resources().has<ParticlePools>());
    CHECK(&other.resources().get<ParticlePools>() != &h.pools());
    // ParticleEmitter is saved and snapshotted but not hashed; ParticleRuntime is runtime-only.
    const ComponentInfo &em = ComponentRegistry::global().info<ParticleEmitter>();
    CHECK((em.flags & kSerialize) != 0);
    CHECK((em.flags & kSnapshot) != 0);
    CHECK((em.flags & kHash) == 0);
    CHECK(ComponentRegistry::global().info<ParticleRuntime>().flags == 0);
}

TEST_CASE("plugin: an emitter acquires a pool and emits at its rate") {
    ParticlesHarness h;
    const Entity e = h.emitter(steady_config(1000, 30, 10), {40, 20});
    CHECK_FALSE(h.world().has<ParticleRuntime>(e));
    h.frames(1);
    REQUIRE(h.world().has<ParticleRuntime>(e));
    const ParticlePoolId pool = h.pool_of(e);
    CHECK(h.pools().valid(pool));
    h.frames(59);
    CHECK(h.pools().alive(pool) == doctest::Approx(30).epsilon(0.05));
    // Particles start at the emitter (streams are pre-moved by at most one frame).
    for (const auto &p : h.pools().particles(pool)) {
        CHECK(p.position.x >= 40.0f - 10.0f * p.age - 1e-3f);
        CHECK(p.position.x <= 40.0f + 10.0f * p.age + 1e-3f);
    }
    // Inactive emitters emit nothing more.
    h.world().get<ParticleEmitter>(e).active = false;
    const std::uint32_t before = h.pools().alive(pool);
    h.frames(10);
    CHECK(h.pools().alive(pool) == before);
    CHECK(h.world().get<ParticleRuntime>(e).accumulator == 0.0f);
}

TEST_CASE("plugin: pending_burst is emitted once; the config burst fires on activation") {
    ParticlesHarness h;
    EmitterConfig c = steady_config(100, 0, 10);
    c.burst = 5;
    const Entity e = h.emitter(c);
    h.frames(1);
    const ParticlePoolId pool = h.pool_of(e);
    CHECK(h.pools().alive(pool) == 5);
    h.frames(3);
    CHECK(h.pools().alive(pool) == 5);
    burst(h.world().get<ParticleEmitter>(e), 3);
    h.frames(1);
    CHECK(h.pools().alive(pool) == 8);
    CHECK(h.world().get<ParticleEmitter>(e).pending_burst == 0);
    h.world().get<ParticleEmitter>(e).active = false;
    h.frames(1);
    burst(h.world().get<ParticleEmitter>(e), 2); // pending bursts work while inactive too
    h.frames(1);
    CHECK(h.pools().alive(pool) == 10);
    h.world().get<ParticleEmitter>(e).active = true; // re-activation fires the config burst
    h.frames(1);
    CHECK(h.pools().alive(pool) == 15);
    // An emitter that starts inactive fires nothing until it is activated.
    const Entity idle = h.emitter(c, {}, false);
    h.frames(2);
    CHECK(h.pools().alive(h.pool_of(idle)) == 0);
}

TEST_CASE("plugin: BurstAt from a tick system is emitted in Update through a shared pool") {
    ParticlesHarness h;
    EmitterConfig c = steady_config(256, 0, 10);
    c.burst = 7;
    const AssetId<EmitterConfig> id = h.assets().add(c);
    add_burst_system(h.app(), BurstPlan{.config = id, .count = 4, .position = {12, 34}});
    h.frames(3);
    const ParticlePoolStats st = h.pools().stats();
    CHECK(st.pools == 1); // one shared pool per config
    CHECK(st.alive == 12);
    // count 0 means the config's burst.
    h.app().res<BurstPlan>().count = 0;
    h.frames(1);
    CHECK(h.pools().stats().alive == 19);
    // A config that is not loaded is skipped.
    h.app().res<BurstPlan>().config = AssetId<EmitterConfig>{999, 1};
    h.frames(2);
    CHECK(h.pools().stats().alive == 19);
    CHECK(h.pools().stats().pools == 1);
}

TEST_CASE("plugin: BurstAt positions are world positions even for local-space configs") {
    ParticlesHarness h;
    EmitterConfig c = steady_config(64, 0, 10);
    c.local_space = true;
    c.speed_min = c.speed_max = 0;
    const AssetId<EmitterConfig> id = h.assets().add(c);
    add_burst_system(h.app(), BurstPlan{.config = id, .count = 1, .position = {100, 50}, .every = 1000});
    h.frames(1);
    REQUIRE(h.pools().stats().alive == 1);
    // The emitter of the same config is a separate, local-space pool.
    const Entity e = h.emitter_for(id, {5, 5});
    h.frames(1);
    CHECK(h.pools().stats().pools == 2);
    CHECK(h.pools().config(h.pool_of(e))->local_space);
}

TEST_CASE("plugin: destroying the emitter's entity lets its particles fade out") {
    ParticlesHarness h;
    EmitterConfig c = steady_config(64, 0, 0.5f);
    c.burst = 10;
    const Entity e = h.emitter(c);
    h.frames(1);
    const ParticlePoolId pool = h.pool_of(e);
    REQUIRE(h.pools().alive(pool) == 10);
    h.world().destroy(e);
    CHECK(h.pools().valid(pool)); // draining
    CHECK(h.pools().alive(pool) == 10);
    h.frames(40);
    CHECK_FALSE(h.pools().valid(pool));
    CHECK(h.pools().stats().pools == 0);
    CHECK(h.pools().stats().reserved == 0);
}

TEST_CASE("plugin: removing ParticleEmitter releases the pool and the runtime") {
    ParticlesHarness h;
    const Entity e = h.emitter(steady_config(64, 60, 0.2f));
    h.frames(5);
    const ParticlePoolId pool = h.pool_of(e);
    REQUIRE(h.pools().alive(pool) > 0);
    h.world().remove<ParticleEmitter>(e);
    h.frames(1);
    CHECK_FALSE(h.world().has<ParticleRuntime>(e));
    h.frames(30);
    CHECK_FALSE(h.pools().valid(pool));
}

TEST_CASE("plugin: a pool destroyed behind the plugin's back is re-acquired") {
    ParticlesHarness h;
    const Entity e = h.emitter(steady_config(64, 60, 1));
    h.frames(2);
    const ParticlePoolId pool = h.pool_of(e);
    h.pools().destroy(pool);
    h.frames(1); // the stale runtime is dropped
    h.frames(1); // and a new pool acquired
    REQUIRE(h.world().has<ParticleRuntime>(e));
    CHECK(h.pools().valid(h.pool_of(e)));
    CHECK_FALSE(h.pool_of(e) == pool);
}

TEST_CASE("plugin: config edits apply in place; switching assets swaps the config") {
    ParticlesHarness h;
    const AssetId<EmitterConfig> a = h.assets().add(steady_config(64, 30, 10));
    const Entity e = h.emitter_for(a);
    h.frames(10);
    const ParticlePoolId pool = h.pool_of(e);
    const std::uint32_t alive = h.pools().alive(pool);
    REQUIRE(alive > 0);
    // Edit the asset: more capacity, a new colour. The pool keeps its particles.
    EmitterConfig *cfg = h.assets().get_mut(a);
    cfg->capacity = 128;
    cfg->color_start = Color{1, 2, 3, 255};
    h.assets().mark_modified(a);
    h.frames(1);
    CHECK(h.pool_of(e) == pool);
    CHECK(h.pools().capacity(pool) == 128);
    CHECK(h.pools().config(pool)->color_start == Color{1, 2, 3, 255});
    CHECK(h.pools().alive(pool) >= alive);
    // Switch the emitter to another asset.
    EmitterConfig other = steady_config(32, 0, 10);
    other.drag = 4;
    h.world().get<ParticleEmitter>(e).config = h.assets().add(other);
    h.frames(1);
    CHECK(h.pools().config(pool)->drag == 4.0f);
    CHECK(h.pools().capacity(pool) == 32);
}

TEST_CASE("plugin: an emitter waits for its config; a failed config never acquires") {
    lam::test::TempDir dir;
    ParticlesHarness h(AssetsConfig{.root = dir.path()});
    const AssetId<EmitterConfig> missing = h.assets().load<EmitterConfig>("nope.particles.json");
    const Entity e = h.emitter_for(missing);
    const Entity none = h.emitter_for({});
    h.frames(5);
    CHECK_FALSE(h.world().has<ParticleRuntime>(e));
    CHECK_FALSE(h.world().has<ParticleRuntime>(none));
    CHECK(h.pools().stats().pools == 0);
    // Once it points at a Ready config, it acquires.
    h.world().get<ParticleEmitter>(e).config = h.assets().add(steady_config());
    h.frames(1);
    CHECK(h.world().has<ParticleRuntime>(e));
}

TEST_CASE("plugin: the emitter follows its interpolated Transform and rotation") {
    ParticlesHarness h;
    EmitterConfig c = steady_config(64, 0, 10);
    c.speed_min = c.speed_max = 10;
    c.spread = 0;
    c.angle = 0;
    c.burst = 1;
    const Entity e = h.emitter(c, {0, 0});
    h.world().get<Transform>(e).rotation = kPi * 0.5f;
    h.world().get<Transform>(e).position = {30, 40};
    h.world().get<PrevTransform>(e) = PrevTransform{.position = {30, 40}, .rotation = kPi * 0.5f};
    h.frames(1);
    const auto ps = h.pools().particles(h.pool_of(e));
    REQUIRE(ps.size() == 1);
    CHECK(ps[0].position.x == doctest::Approx(30.0f));
    CHECK(ps[0].position.y == doctest::Approx(40.0f));
    CHECK(ps[0].velocity.x == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(ps[0].velocity.y == doctest::Approx(10.0f).epsilon(1e-3));
}

TEST_CASE("plugin: a moving emitter spreads its stream along its path") {
    ParticlesHarness h;
    EmitterConfig c = steady_config(256, 600, 10); // 10 per frame
    c.speed_min = c.speed_max = 0;
    const Entity e = h.emitter(c, {0, 0});
    h.frames(1);
    h.world().get<Transform>(e).position = {100, 0};
    h.world().get<PrevTransform>(e).position = {100, 0};
    h.frames(1);
    std::vector<float> xs;
    for (const auto &p : h.pools().particles(h.pool_of(e)))
        xs.push_back(p.position.x);
    std::sort(xs.begin(), xs.end());
    // The second frame's ten particles sit between x = 0 and x = 100, not all at 100.
    const auto between = std::count_if(xs.begin(), xs.end(), [](float x) { return x > 1.0f && x < 99.0f; });
    CHECK(between >= 8);
}

TEST_CASE("plugin: local-space particles move with the emitter") {
    ParticlesHarness h;
    EmitterConfig c = steady_config(64, 0, 10);
    c.speed_min = c.speed_max = 0;
    c.local_space = true;
    c.burst = 3;
    const Entity e = h.emitter(c, {10, 10});
    h.frames(1);
    h.world().get<Transform>(e).position = {200, 10};
    h.world().get<PrevTransform>(e).position = {200, 10};
    RenderQueue q(16);
    h.frames(1);
    h.pools().draw(q, Rect{-1000, -1000, 4000, 4000});
    REQUIRE(q.size() == 3);
    const QuadVertex &v = q.quads()[0].v[0];
    CHECK(v.pos.x == doctest::Approx(199.0f)); // 2 px quad centred on the emitter
}

TEST_CASE("plugin: prewarm fills a stream when the emitter starts") {
    ParticlesHarness h;
    EmitterConfig c = steady_config(512, 100, 2.0f);
    c.prewarm = 5;
    const Entity e = h.emitter(c);
    h.frames(1);
    // Window = min(prewarm, life_max) = 2 s at 100/s, plus this frame's stream.
    CHECK(h.pools().alive(h.pool_of(e)) == doctest::Approx(200).epsilon(0.03));
    const Entity rain = h.emitter(rain_preset(1.0f));
    h.frames(1);
    CHECK(h.pools().alive(h.pool_of(rain)) > 150);
}

TEST_CASE("plugin: restore prunes runtimes of dead entities and emitters re-acquire") {
    ParticlesHarness h;
    World &w = h.world();
    const Entity a = h.emitter(steady_config(64, 30, 10));
    h.frames(2);
    const ParticlePoolId pool_a = h.pool_of(a);
    WorldSnapshot snap;
    w.snapshot_into(snap);
    SUBCASE("an entity created after the snapshot loses its pool") {
        const Entity b = h.emitter(steady_config(64, 30, 10));
        h.frames(2);
        const ParticlePoolId pool_b = h.pool_of(b);
        REQUIRE(h.pools().valid(pool_b));
        w.restore(snap);
        CHECK_FALSE(w.alive(b));
        CHECK(h.pool_of(a) == pool_a); // survivors keep their runtime and pool
        CHECK(h.pools().valid(pool_a));
        h.frames(1);
        const std::uint32_t left = h.pools().alive(pool_b); // released: it only drains now
        h.frames(10);
        CHECK(h.pools().alive(pool_b) <= left);
        h.frames(700);
        CHECK_FALSE(h.pools().valid(pool_b));
        CHECK(h.pools().stats().reserved == 64);
    }
    SUBCASE("a destroyed entity comes back and re-acquires") {
        w.destroy(a);
        h.frames(1);
        w.restore(snap);
        REQUIRE(w.alive(a));
        CHECK(w.has<ParticleEmitter>(a));
        CHECK_FALSE(w.has<ParticleRuntime>(a)); // pruned (runtime-only)
        h.frames(1);
        REQUIRE(w.has<ParticleRuntime>(a));
        CHECK(h.pools().valid(h.pool_of(a)));
        CHECK_FALSE(h.pool_of(a) == pool_a);
    }
    SUBCASE("a recycled index does not keep the old pool") {
        w.destroy(a);
        const Entity c = h.emitter(steady_config(64, 30, 10)); // may reuse a's index
        h.frames(1);
        const ParticlePoolId pool_c = h.pool_of(c);
        w.restore(snap);
        CHECK(w.alive(a));
        CHECK_FALSE(w.alive(c));
        CHECK_FALSE(w.has<ParticleRuntime>(a));
        h.frames(1);
        CHECK(w.has<ParticleRuntime>(a));
        CHECK_FALSE(h.pool_of(a) == pool_c);
        CHECK(h.pools().stats().reserved <= 3u * 64u);
    }
    SUBCASE("the restored pending burst is emitted") {
        burst(w.get<ParticleEmitter>(a), 9);
        WorldSnapshot with_burst;
        w.snapshot_into(with_burst);
        h.frames(1);
        CHECK(w.get<ParticleEmitter>(a).pending_burst == 0);
        w.restore(with_burst);
        CHECK(w.get<ParticleEmitter>(a).pending_burst == 9);
    }
}

TEST_CASE("plugin: an exhausted budget makes emitters wait and warns once") {
    lam::test::CaptureLog log;
    ParticlesHarness h({}, ParticlesConfig{.max_particles_per_world = 100, .max_pools = 8});
    const Entity a = h.emitter(steady_config(60, 60, 0.1f));
    const Entity b = h.emitter(steady_config(60, 60, 0.1f));
    const Entity c = h.emitter(steady_config(60, 60, 0.1f));
    h.frames(5);
    CHECK(h.world().has<ParticleRuntime>(a));
    CHECK_FALSE(h.world().has<ParticleRuntime>(b));
    CHECK_FALSE(h.world().has<ParticleRuntime>(c));
    CHECK(log.count(LogLevel::Warn) == 1);
    CHECK(log.contains("budget"));
    h.world().destroy(a);
    h.frames(20); // a's pool drains, then one waiting emitter (dense order) gets the budget
    CHECK(h.world().has<ParticleRuntime>(b) != h.world().has<ParticleRuntime>(c));
    CHECK(log.count(LogLevel::Warn) == 1);
}

TEST_CASE("plugin: draw_particles pushes the world's particles into the RenderQueue") {
    ParticlesHarness h;
    EmitterConfig c = steady_config(64, 0, 10);
    c.burst = 6;
    c.speed_min = c.speed_max = 0;
    h.emitter(c, {0, 0});
    h.frames(2);
    CHECK(h.last_quads() == 6);
    CHECK(h.pools().stats().drawn == 6);
    // Screen-space weather needs no Transform: the emitter sits at the virtual origin.
    const Entity weather = h.world().create();
    h.world().add<ParticleEmitter>(weather, ParticleEmitter{.config = h.assets().add(rain_preset(1.0f))});
    h.frames(2);
    CHECK(h.last_quads() > 100);
}

TEST_CASE("plugin: the same App seed gives the same particles") {
    const auto run = [](std::uint64_t seed) {
        ParticlesHarness h({}, {}, AppConfig{.seed = seed});
        EmitterConfig c = steady_config(256, 90, 1);
        c.speed_min = 10;
        c.speed_max = 80;
        c.jitter = 20;
        c.burst = 12;
        const Entity e = h.emitter(c, {5, 5});
        h.frames(30);
        std::vector<Vec2> out;
        for (const auto &p : h.pools().particles(h.pool_of(e)))
            out.push_back(p.position);
        return out;
    };
    const auto a = run(7), b = run(7), c = run(8);
    REQUIRE(a.size() == b.size());
    CHECK(a == b);
    CHECK_FALSE(a == c);
}

TEST_CASE("plugin: no allocation in steady state (emitters, bursts, BurstAt, churn, draw)") {
    ParticlesHarness h;
    World &w = h.world();
    EmitterConfig trail = steady_config(256, 120, 0.5f);
    trail.drag = 1;
    trail.gravity = {0, 40};
    trail.jitter = 5;
    const AssetId<EmitterConfig> trail_id = h.assets().add(trail);
    EmitterConfig pop = steady_config(256, 0, 0.4f);
    pop.burst = 16;
    const AssetId<EmitterConfig> pop_id = h.assets().add(pop);
    for (int i = 0; i < 4; ++i)
        h.emitter_for(trail_id, {static_cast<float>(i * 30), 0});
    h.emitter(rain_preset(0.6f));
    add_burst_system(h.app(), BurstPlan{.config = pop_id, .count = 8, .position = {50, 50}, .every = 5});
    // Churn: an emitter entity is replaced every 20 frames.
    struct Churn {
        AssetId<EmitterConfig> config;
        Entity current;
        int frame = 0;
    };
    h.app().insert_resource<Churn>(Churn{.config = pop_id});
    h.app().add_system(Stage::Simulate, +[](Ctx &ctx) {
        Churn &c = ctx.res<Churn>();
        if (c.frame++ % 20 != 0) return;
        World &world = ctx.world();
        if (world.alive(c.current)) world.destroy(c.current);
        c.current = world.create();
        world.add<Transform>(c.current, Transform{.position = {10, 10}});
        world.add<ParticleEmitter>(c.current, ParticleEmitter{.config = c.config, .pending_burst = 4});
    }, "test.churn");
    (void)w;
    h.frames(240);
    CHECK(h.pools().stats().alive > 0);
    CHECK(h.last_quads() > 0);
    CHECK_NO_ALLOC(h.frames(600));
}

// ---- Regressions --------------------------------------------------------------------------------

namespace {
// Particles of `e`'s pool strictly between x = 100 and x = 900 (a streak across a 0 -> 1000 jump).
std::size_t streaked(ParticlesHarness &h, Entity e) {
    std::size_t n = 0;
    for (const auto &p : h.pools().particles(h.pool_of(e)))
        n += (p.position.x > 100.0f && p.position.x < 900.0f) ? 1 : 0;
    return n;
}
} // namespace

TEST_CASE("a jump (teleport, restore) never streaks the stream across the gap") {
    ParticlesHarness h;
    World &w = h.world();
    EmitterConfig c = steady_config(4096, 6000, 1.0f); // 100 per frame, motionless
    c.speed_min = c.speed_max = 0;
    const Entity e = h.emitter(c, {0, 0});
    h.frames(5);
    SUBCASE("teleport") {
        w.teleport(e, {1000, 0});
        h.frames(1);
        CHECK(streaked(h, e) == 0);                             // was ~80
        CHECK(h.pools().alive(h.pool_of(e)) > 400);             // this frame's stream still ran
    }
    SUBCASE("restore of a surviving emitter") {
        w.teleport(e, {1000, 0});
        h.frames(2);
        WorldSnapshot at_far;
        w.snapshot_into(at_far);
        w.teleport(e, {0, 0});
        h.frames(2);
        REQUIRE(streaked(h, e) == 0);
        const ParticlePoolId pool = h.pool_of(e);
        w.restore(at_far);                                      // back at x = 1000, same pool
        REQUIRE(h.pool_of(e) == pool);
        h.frames(1);
        CHECK(streaked(h, e) == 0);                             // was ~80
    }
    SUBCASE("ordinary movement still spreads the stream along the path") {
        w.get<Transform>(e).position = {1000, 0};
        w.get<PrevTransform>(e).position = {1000, 0};
        h.frames(1);
        CHECK(streaked(h, e) > 50);
    }
}
