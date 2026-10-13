// ParticlePools: the slab, budgets, ids, integration, streams and determinism.
#include <cmath>
#include <cstdint>
#include <vector>

#include <lam/math/rng.h>
#include <lam/particles/particles.h>
#include <lam/test/support.h>

#include "particles_test_util.h"

using namespace lam;
using lam::test::steady_config;

namespace {

float max_abs_diff(std::span<const ParticlePools::Particle> a, std::span<const ParticlePools::Particle> b) {
    float d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        d = std::max(d, std::fabs(a[i].position.x - b[i].position.x));
        d = std::max(d, std::fabs(a[i].position.y - b[i].position.y));
        d = std::max(d, std::fabs(a[i].velocity.x - b[i].velocity.x));
        d = std::max(d, std::fabs(a[i].velocity.y - b[i].velocity.y));
    }
    return d;
}

} // namespace

TEST_CASE("particles: bursts are pooled, age and die") {
    ParticlePools pools;
    Rng rng(7);
    EmitterConfig burst_config;
    burst_config.capacity = 16;
    burst_config.life_min = burst_config.life_max = 0.5f;
    const ParticlePoolId p = pools.create(burst_config);
    REQUIRE(p);
    pools.emit(p, {0, 0}, 10, rng);
    CHECK(pools.alive(p) == 10);
    pools.emit(p, {0, 0}, 10, rng); // over capacity: the pool stays full, never grows
    CHECK(pools.alive(p) == 16);
    CHECK(pools.stats().replaced == 4);
    pools.update(0.25f, rng);
    CHECK(pools.alive(p) == 16);
    // Half way through life: size half way from 2 to 0 (checked through the draw in test_draw).
    for (const ParticlePools::Particle &q : pools.particles(p))
        CHECK(q.age / q.life == doctest::Approx(0.5f));
    pools.update(0.3f, rng);
    CHECK(pools.alive(p) == 0);
    CHECK(pools.total_alive() == 0);
    CHECK(pools.valid(p)); // an empty pool lives on until destroyed
}

TEST_CASE("particles: the same seed gives the same burst") {
    ParticlePools a, b;
    Rng ra(3), rb(3);
    const ParticlePoolId pa = a.create({});
    const ParticlePoolId pb = b.create({});
    a.emit(pa, {5, 5}, 8, ra);
    b.emit(pb, {5, 5}, 8, rb);
    REQUIRE(a.alive(pa) == 8);
    REQUIRE(b.alive(pb) == 8);
    const auto qa = a.particles(pa);
    const auto qb = b.particles(pb);
    for (std::size_t i = 0; i < qa.size(); ++i) {
        CHECK(qa[i].velocity == qb[i].velocity);
        CHECK(qa[i].position == qb[i].position);
        CHECK(qa[i].life == qb[i].life);
    }
    // ...and the same frames after it, jitter included.
    EmitterConfig noisy;
    noisy.jitter = 50;
    noisy.drag = 2;
    noisy.gravity = {0, 40};
    const ParticlePoolId na = a.create(noisy);
    const ParticlePoolId nb = b.create(noisy);
    a.emit(na, {}, 32, ra);
    b.emit(nb, {}, 32, rb);
    for (int i = 0; i < 20; ++i) {
        a.update(1.0f / 60.0f, ra);
        b.update(1.0f / 60.0f, rb);
    }
    CHECK(max_abs_diff(a.particles(na), b.particles(nb)) == 0.0f);
    // A different seed differs.
    ParticlePools c;
    Rng rc(4);
    const ParticlePoolId pc = c.create({});
    c.emit(pc, {5, 5}, 8, rc);
    CHECK_FALSE(c.particles(pc)[0].velocity == qa[0].velocity);
}

TEST_CASE("emit: speed, angle, spread and life stay inside their ranges") {
    ParticlePools pools;
    Rng rng(11);
    EmitterConfig c;
    c.capacity = 2000;
    c.speed_min = 30;
    c.speed_max = 40;
    c.angle = kPi * 0.5f; // down
    c.spread = 0.5f;
    c.life_min = 1;
    c.life_max = 2;
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {100, 50}, 2000, rng);
    REQUIRE(pools.alive(p) == 2000);
    for (const ParticlePools::Particle &q : pools.particles(p)) {
        const float speed = length(q.velocity);
        CHECK(speed >= 30.0f - 1e-3f);
        CHECK(speed < 40.0f + 1e-3f);
        const float a = det_atan2(q.velocity.y, q.velocity.x);
        CHECK(a >= kPi * 0.5f - 0.25f - 1e-4f);
        CHECK(a <= kPi * 0.5f + 0.25f + 1e-4f);
        CHECK(q.life >= 1.0f);
        CHECK(q.life < 2.0f);
        CHECK(q.position == Vec2{100, 50}); // a point emitter
        CHECK(q.age == 0.0f);
    }
}

TEST_CASE("emit: spawn shapes stay inside the spawn area") {
    ParticlePools pools;
    Rng rng(5);
    EmitterConfig c;
    c.capacity = 1000;
    c.spawn_area = {-10, -20, 20, 40}; // centred on the emitter
    SUBCASE("rect") {
        c.spawn_shape = SpawnShape::Rect;
        const ParticlePoolId p = pools.create(c);
        pools.emit(p, {50, 50}, 1000, rng);
        float min_x = 1e9f, max_x = -1e9f;
        for (const auto &q : pools.particles(p)) {
            CHECK(q.position.x >= 40.0f);
            CHECK(q.position.x < 60.0f);
            CHECK(q.position.y >= 30.0f);
            CHECK(q.position.y < 70.0f);
            min_x = std::min(min_x, q.position.x);
            max_x = std::max(max_x, q.position.x);
        }
        CHECK(max_x - min_x > 18.0f); // it spreads
    }
    SUBCASE("ellipse") {
        c.spawn_shape = SpawnShape::Ellipse;
        const ParticlePoolId p = pools.create(c);
        pools.emit(p, {0, 0}, 1000, rng);
        for (const auto &q : pools.particles(p)) {
            const float e = (q.position.x / 10) * (q.position.x / 10) + (q.position.y / 20) * (q.position.y / 20);
            CHECK(e <= 1.0f + 1e-3f);
        }
    }
    SUBCASE("ring") {
        c.spawn_shape = SpawnShape::Ring;
        const ParticlePoolId p = pools.create(c);
        pools.emit(p, {0, 0}, 1000, rng);
        for (const auto &q : pools.particles(p)) {
            const float e = (q.position.x / 10) * (q.position.x / 10) + (q.position.y / 20) * (q.position.y / 20);
            CHECK(e == doctest::Approx(1.0f).epsilon(1e-3));
        }
    }
    SUBCASE("a line (zero height) spreads along x only") {
        c.spawn_area = {0, 5, 100, 0};
        const ParticlePoolId p = pools.create(c);
        pools.emit(p, {0, 0}, 200, rng);
        for (const auto &q : pools.particles(p))
            CHECK(q.position.y == 5.0f);
    }
}

TEST_CASE("emit: the anchor rotation turns the emission angle and the spawn area") {
    ParticlePools pools;
    Rng rng(2);
    EmitterConfig c;
    c.spread = 0;
    c.speed_min = c.speed_max = 10;
    c.spawn_area = {10, 0, 0, 0}; // a point 10 px to the right of the emitter
    const ParticlePoolId p = pools.create(c);
    pools.set_origin(p, {0, 0}, kPi * 0.5f);
    pools.emit(p, {100, 100}, 1, rng);
    const auto q = pools.particles(p)[0];
    CHECK(q.position.x == doctest::Approx(100.0f).epsilon(1e-4));
    CHECK(q.position.y == doctest::Approx(110.0f).epsilon(1e-4));
    CHECK(q.velocity.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(q.velocity.y == doctest::Approx(10.0f).epsilon(1e-4));
}

TEST_CASE("update: dead particles are swap-removed; the rest keep moving") {
    ParticlePools pools;
    Rng rng(1);
    EmitterConfig c = steady_config(8, 0, 1.0f);
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {}, 4, rng);
    pools.update(0.5f, rng);
    c.life_min = c.life_max = 3.0f;
    REQUIRE(pools.set_config(p, c));
    pools.emit(p, {}, 4, rng); // 4 short-lived (age 0.5) + 4 long-lived (age 0)
    pools.update(0.6f, rng);   // the first four die
    CHECK(pools.alive(p) == 4);
    for (const auto &q : pools.particles(p)) {
        CHECK(q.life == 3.0f);
        CHECK(q.age == doctest::Approx(0.6f));
    }
    // Non-finite or non-positive dt changes nothing.
    const auto before = pools.particles(p)[0];
    pools.update(0.0f, rng);
    pools.update(-1.0f, rng);
    pools.update(std::numeric_limits<float>::quiet_NaN(), rng);
    pools.update(std::numeric_limits<float>::infinity(), rng);
    CHECK(pools.particles(p)[0].age == before.age);
    CHECK(pools.particles(p)[0].position == before.position);
}

TEST_CASE("update: gravity and exponential drag integrate exactly (frame-rate independent)") {
    EmitterConfig c;
    c.capacity = 64;
    c.life_min = c.life_max = 100;
    c.speed_min = 40;
    c.speed_max = 120;
    c.gravity = {5, 90};
    for (const float drag : {0.0f, 0.7f, 6.0f}) {
        CAPTURE(drag);
        c.drag = drag;
        ParticlePools a, b, d;
        Rng ra(8), rb(8), rd(8);
        const ParticlePoolId pa = a.create(c), pb = b.create(c), pd = d.create(c);
        a.emit(pa, {}, 64, ra);
        b.emit(pb, {}, 64, rb);
        d.emit(pd, {}, 64, rd);
        for (int i = 0; i < 30; ++i)
            a.update(1.0f / 30.0f, ra);
        for (int i = 0; i < 144; ++i)
            b.update(1.0f / 144.0f, rb);
        d.update(1.0f, rd);
        CHECK(max_abs_diff(a.particles(pa), b.particles(pb)) < 0.02f);
        CHECK(max_abs_diff(a.particles(pa), d.particles(pd)) < 0.02f);
        // Closed form for one particle: v(t) = vt + (v0 - vt) e^(-dt) with vt = g / d.
        if (drag > 0) {
            Rng r0(8);
            ParticlePools e;
            const ParticlePoolId pe = e.create(c);
            e.emit(pe, {}, 1, r0);
            const Vec2 v0 = e.particles(pe)[0].velocity;
            e.update(1.0f, r0);
            const Vec2 vt = c.gravity / drag;
            const float k = std::exp(-drag);
            CHECK(e.particles(pe)[0].velocity.x == doctest::Approx(vt.x + (v0.x - vt.x) * k).epsilon(1e-4));
            CHECK(e.particles(pe)[0].velocity.y == doctest::Approx(vt.y + (v0.y - vt.y) * k).epsilon(1e-4));
        }
    }
}

TEST_CASE("update: without drag the motion is ballistic") {
    ParticlePools pools;
    Rng rng(4);
    EmitterConfig c = steady_config(4, 0, 10);
    c.spread = 0;
    c.angle = 0;
    c.gravity = {0, 10};
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {0, 0}, 1, rng);
    pools.update(2.0f, rng);
    const auto q = pools.particles(p)[0];
    CHECK(q.position.x == doctest::Approx(20.0f));
    CHECK(q.position.y == doctest::Approx(20.0f)); // g t^2 / 2
    CHECK(q.velocity.y == doctest::Approx(20.0f));
}

TEST_CASE("update: spin turns particles") {
    ParticlePools pools;
    Rng rng(4);
    EmitterConfig c = steady_config(4, 0, 10);
    c.rotation_min = c.rotation_max = 0.5f;
    c.spin_min = c.spin_max = 2.0f;
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {}, 1, rng);
    CHECK(pools.particles(p)[0].rotation == 0.5f);
    pools.update(0.25f, rng);
    CHECK(pools.particles(p)[0].rotation == doctest::Approx(1.0f));
}

TEST_CASE("emit_stream: births spread over the interval and are pre-aged") {
    ParticlePools pools;
    Rng rng(6);
    EmitterConfig c = steady_config(64, 0, 10);
    c.spread = 0;
    c.angle = 0;
    c.speed_min = c.speed_max = 100;
    const ParticlePoolId p = pools.create(c);
    // 4 births over 0.1 s while the emitter moves from x = 0 to x = 40.
    pools.emit_stream(p, {0, 0}, {40, 0}, 4, 0.1f, rng);
    REQUIRE(pools.alive(p) == 4);
    std::vector<float> ages, xs;
    for (const auto &q : pools.particles(p)) {
        ages.push_back(q.age);
        xs.push_back(q.position.x);
    }
    CHECK(ages[0] == doctest::Approx(0.0875f));
    CHECK(ages[3] == doctest::Approx(0.0125f));
    // Born at x = 5 (1/8 of the way) and moved 100 px/s for 0.0875 s.
    CHECK(xs[0] == doctest::Approx(5.0f + 8.75f));
    CHECK(xs[3] == doctest::Approx(35.0f + 1.25f));
    // Births already dead within the interval are dropped.
    EmitterConfig quick = steady_config(64, 0, 0.05f);
    const ParticlePoolId q = pools.create(quick);
    pools.emit_stream(q, {}, {}, 10, 0.1f, rng);
    CHECK(pools.alive(q) == 5);
    // More births than capacity: only the newest `capacity` are considered.
    EmitterConfig small = steady_config(3, 0, 10);
    const ParticlePoolId s = pools.create(small);
    pools.emit_stream(s, {}, {}, 1000, 1.0f, rng);
    CHECK(pools.alive(s) == 3);
    for (const auto &r : pools.particles(s))
        CHECK(r.age < 0.0035f);
}

TEST_CASE("emit_stream: a stream looks the same at any frame rate") {
    // The same 1 s stream at 30 and 120 fps ends with the same number of particles in the same
    // region (positions depend on random draws, so compare the extent).
    const auto run = [](int fps) {
        ParticlePools pools;
        Rng rng(10);
        EmitterConfig c = steady_config(1000, 200, 0.5f);
        c.spread = 0;
        c.speed_min = c.speed_max = 100;
        const ParticlePoolId p = pools.create(c);
        const float dt = 1.0f / static_cast<float>(fps);
        float acc = 0;
        for (int i = 0; i < fps; ++i) {
            pools.update(dt, rng);
            acc += c.rate * dt;
            const auto n = static_cast<std::uint32_t>(acc + 1e-4f);
            acc -= static_cast<float>(n);
            pools.emit_stream(p, {}, {}, n, dt, rng);
        }
        float max_x = 0;
        for (const auto &q : pools.particles(p))
            max_x = std::max(max_x, q.position.x);
        return std::pair<std::uint32_t, float>{pools.alive(p), max_x};
    };
    const auto a = run(30);
    const auto b = run(120);
    CHECK(a.first == doctest::Approx(100).epsilon(0.03));
    CHECK(b.first == doctest::Approx(100).epsilon(0.03));
    CHECK(a.second == doctest::Approx(50.0f).epsilon(0.05)); // the oldest are almost 0.5 s old
    CHECK(b.second == doctest::Approx(50.0f).epsilon(0.05));
}

TEST_CASE("budget: create refuses pools over max_particles or max_pools, and counts it") {
    ParticlePools pools(100, 3);
    CHECK(pools.max_particles() == 100);
    CHECK(pools.max_pools() == 3);
    const ParticlePoolId a = pools.create(steady_config(60));
    REQUIRE(a);
    CHECK_FALSE(pools.create(steady_config(41)));
    CHECK(pools.stats().rejected == 1);
    const ParticlePoolId b = pools.create(steady_config(40));
    REQUIRE(b);
    CHECK(pools.stats().reserved == 100);
    pools.destroy(a);
    CHECK(pools.stats().reserved == 40);
    const ParticlePoolId c = pools.create(steady_config(10));
    const ParticlePoolId d = pools.create(steady_config(10));
    REQUIRE(c);
    REQUIRE(d);
    CHECK_FALSE(pools.create(steady_config(1))); // no slot left
    CHECK(pools.stats().rejected == 2);
    CHECK(pools.stats().pools == 3);
}

TEST_CASE("ids: generational; stale and none ids are ignored everywhere") {
    ParticlePools pools(1000, 2);
    Rng rng(1);
    const ParticlePoolId a = pools.create(steady_config(10));
    REQUIRE(a);
    CHECK(a.index == 1);
    CHECK(a.generation == 1);
    pools.destroy(a);
    CHECK_FALSE(pools.valid(a));
    const ParticlePoolId b = pools.create(steady_config(10));
    CHECK(b.index == a.index);
    CHECK(b.generation == 2);
    CHECK_FALSE(a == b);
    // The stale id touches nothing.
    pools.emit(a, {}, 5, rng);
    pools.emit_stream(a, {}, {}, 5, 0.1f, rng);
    pools.set_origin(a, {1, 1});
    CHECK_FALSE(pools.set_config(a, steady_config(20)));
    pools.clear(a);
    pools.release(a);
    pools.destroy(a);
    CHECK(pools.alive(a) == 0);
    CHECK(pools.capacity(a) == 0);
    CHECK(pools.config(a) == nullptr);
    CHECK(pools.particles(a).empty());
    CHECK(pools.valid(b));
    CHECK(pools.alive(b) == 0);
    // None and out-of-range ids.
    CHECK_FALSE(pools.valid({}));
    CHECK_FALSE(pools.valid({99, 1}));
    pools.emit({}, {}, 3, rng);
    pools.destroy({99, 1});
    CHECK(pools.total_alive() == 0);
}

TEST_CASE("release: a pool stops emitting and is destroyed with its last particle") {
    ParticlePools pools;
    Rng rng(1);
    const ParticlePoolId p = pools.create(steady_config(16, 0, 0.5f));
    pools.emit(p, {}, 5, rng);
    pools.release(p);
    CHECK(pools.valid(p));
    pools.emit(p, {}, 5, rng); // ignored while draining
    CHECK(pools.alive(p) == 5);
    pools.update(0.3f, rng);
    CHECK(pools.valid(p));
    pools.update(0.3f, rng);
    CHECK_FALSE(pools.valid(p));
    CHECK(pools.stats().reserved == 0);
    // Releasing an empty pool destroys it at once.
    const ParticlePoolId q = pools.create(steady_config(16));
    pools.release(q);
    CHECK_FALSE(pools.valid(q));
}

TEST_CASE("slab: fragmented pools are packed, particles survive, and the slab stops growing") {
    ParticlePools pools(4096, 64);
    Rng rng(3);
    std::vector<ParticlePoolId> ids;
    for (int i = 0; i < 8; ++i) {
        ids.push_back(pools.create(steady_config(256, 0, 100)));
        pools.emit(ids.back(), {static_cast<float>(i), 0}, 100, rng);
    }
    const std::uint32_t slab = pools.stats().slab_size;
    CHECK(slab >= 2048);
    CHECK(slab <= 4096);
    // Free every other pool, then ask for a large one: it fits only after packing.
    for (int i = 0; i < 8; i += 2)
        pools.destroy(ids[static_cast<std::size_t>(i)]);
    const ParticlePoolId big = pools.create(steady_config(2048, 0, 100));
    REQUIRE(big);
    CHECK(pools.stats().slab_size <= 4096);
    for (int i = 1; i < 8; i += 2) {
        const ParticlePoolId id = ids[static_cast<std::size_t>(i)];
        REQUIRE(pools.alive(id) == 100);
        for (const auto &q : pools.particles(id))
            CHECK(q.position.x == static_cast<float>(i)); // moved with their pool
    }
    // Churn: creating and destroying pools of the working size never grows the slab again.
    const std::uint32_t settled = pools.stats().slab_size;
    for (int round = 0; round < 50; ++round) {
        const ParticlePoolId t = pools.create(steady_config(512, 0, 100));
        REQUIRE(t);
        pools.emit(t, {}, 20, rng);
        pools.destroy(t);
    }
    CHECK(pools.stats().slab_size == settled);
}

TEST_CASE("set_config: grows and shrinks in place or by moving, within budget") {
    ParticlePools pools(300, 8);
    Rng rng(2);
    const ParticlePoolId a = pools.create(steady_config(50, 0, 100));
    const ParticlePoolId b = pools.create(steady_config(50, 0, 100));
    pools.emit(a, {1, 0}, 50, rng);
    pools.emit(b, {2, 0}, 50, rng);
    // Grow a (not on top of the slab): it moves, with its particles.
    REQUIRE(pools.set_config(a, steady_config(150, 0, 100)));
    CHECK(pools.capacity(a) == 150);
    CHECK(pools.alive(a) == 50);
    pools.emit(a, {1, 0}, 100, rng);
    CHECK(pools.alive(a) == 150);
    for (const auto &q : pools.particles(a))
        CHECK(q.position.x == 1.0f);
    for (const auto &q : pools.particles(b))
        CHECK(q.position.x == 2.0f);
    CHECK(pools.stats().reserved == 200);
    // Over budget: nothing changes.
    CHECK_FALSE(pools.set_config(b, steady_config(151, 0, 100)));
    CHECK(pools.capacity(b) == 50);
    CHECK(pools.alive(b) == 50);
    // Growing to exactly the budget works (the slab is packed first).
    REQUIRE(pools.set_config(b, steady_config(150, 0, 100)));
    CHECK(pools.stats().reserved == 300);
    CHECK(pools.stats().slab_size <= 300);
    for (const auto &q : pools.particles(b))
        CHECK(q.position.x == 2.0f);
    // Shrinking keeps what fits.
    REQUIRE(pools.set_config(a, steady_config(10, 0, 100)));
    CHECK(pools.alive(a) == 10);
    CHECK(pools.capacity(a) == 10);
    CHECK(pools.stats().reserved == 160);
    // The config itself is replaced (sanitized).
    EmitterConfig odd = steady_config(10, 0, 100);
    odd.drag = -3;
    REQUIRE(pools.set_config(a, odd));
    CHECK(pools.config(a)->drag == 0.0f);
}

TEST_CASE("local space: particles follow the anchor; switching space keeps them in place") {
    ParticlePools pools;
    Rng rng(4);
    EmitterConfig c = steady_config(8, 0, 100);
    c.local_space = true;
    c.speed_min = c.speed_max = 0;
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {50, 60}, 1, rng);
    CHECK(pools.particles(p)[0].position == Vec2{0, 0}); // relative to the anchor
    pools.set_origin(p, {70, 60});
    c.local_space = false;
    REQUIRE(pools.set_config(p, c));
    CHECK(pools.particles(p)[0].position == Vec2{70, 60});
    c.local_space = true;
    REQUIRE(pools.set_config(p, c));
    CHECK(pools.particles(p)[0].position == Vec2{0, 0});
}

TEST_CASE("clear: kills particles but keeps pools") {
    ParticlePools pools;
    Rng rng(4);
    const ParticlePoolId a = pools.create(steady_config());
    const ParticlePoolId b = pools.create(steady_config());
    pools.emit(a, {}, 10, rng);
    pools.emit(b, {}, 10, rng);
    pools.clear(a);
    CHECK(pools.alive(a) == 0);
    CHECK(pools.alive(b) == 10);
    pools.clear();
    CHECK(pools.total_alive() == 0);
    CHECK(pools.valid(a));
    CHECK(pools.valid(b));
}

TEST_CASE("constructor clamps its limits") {
    ParticlePools huge(0xFFFFFFFFu, 0xFFFFFFFFu);
    CHECK(huge.max_particles() == (1u << 24));
    CHECK(huge.max_pools() == 65535);
    ParticlePools none(0, 0);
    CHECK_FALSE(none.create({}));
    CHECK(none.stats().rejected == 1);
}

TEST_CASE("no allocation: emit, stream, update and pool churn after warm-up") {
    ParticlePools pools(65536, 64);
    Rng rng(12);
    EmitterConfig c;
    c.capacity = 1024;
    c.jitter = 10;
    c.drag = 1;
    c.gravity = {0, 30};
    const auto frame = [&](int i) {
        const ParticlePoolId t = pools.create(c);
        pools.emit(t, {static_cast<float>(i), 0}, 200, rng);
        pools.emit_stream(t, {}, {10, 10}, 300, 1.0f / 60.0f, rng);
        pools.update(1.0f / 60.0f, rng);
        pools.release(t);
    };
    for (int i = 0; i < 120; ++i)
        frame(i);
    CHECK_NO_ALLOC(for (int i = 0; i < 600; ++i) frame(i));
}

TEST_CASE("no allocation canary: a fresh pool's first create grows the slab") {
    if (!allocation_counting_enabled()) return; // asan builds: counting is off
    ParticlePools pools;
    MUST_FAIL(CHECK_NO_ALLOC((void)pools.create(steady_config(512))));
}
