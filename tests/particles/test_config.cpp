// sanitize(), burst() and the weather presets. Hostile values must come out finite and bounded.
#include <cmath>
#include <cstdint>
#include <limits>

#include <lam/math/rng.h>
#include <lam/particles/particles.h>
#include <lam/render/render_queue.h>
#include <lam/test/support.h>

#include "particles_test_util.h"

using namespace lam;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

bool all_finite(const EmitterConfig &c) {
    const float values[] = {c.rate,         c.speed_min,    c.speed_max,     c.angle,       c.spread,
                            c.life_min,     c.life_max,     c.size_start,    c.size_end,    c.gravity.x,
                            c.gravity.y,    c.drag,         c.region.source.x, c.region.source.y,
                            c.region.source.w, c.region.source.h, c.region.size.x, c.region.size.y,
                            c.spawn_area.x, c.spawn_area.y, c.spawn_area.w,  c.spawn_area.h,
                            c.rotation_min, c.rotation_max, c.spin_min,      c.spin_max,    c.stretch,
                            c.sway,         c.sway_hz,      c.jitter,        c.depth,       c.prewarm};
    for (const float v : values) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

EmitterConfig hostile(float v) {
    EmitterConfig c;
    c.capacity = 0xFFFFFFFFu;
    c.rate = v;
    c.burst = 65535;
    c.speed_min = v;
    c.speed_max = -v;
    c.angle = v;
    c.spread = v;
    c.life_min = v;
    c.life_max = v;
    c.size_start = v;
    c.size_end = -v;
    c.gravity = {v, -v};
    c.drag = v;
    c.region.source = {v, v, -v, v};
    c.region.size = {v, v};
    c.region.trim_offset = {v, v};
    c.material = 200;
    c.spawn_area = {v, v, v, -v};
    c.spawn_shape = static_cast<SpawnShape>(77);
    c.rotation_min = v;
    c.rotation_max = -v;
    c.spin_min = v;
    c.spin_max = v;
    c.stretch = v;
    c.sway = v;
    c.sway_hz = v;
    c.jitter = v;
    c.depth = v;
    c.prewarm = v;
    return c;
}

} // namespace

TEST_CASE("sanitize: the default config is unchanged") {
    const EmitterConfig d{};
    const EmitterConfig s = sanitize(d);
    CHECK(s.capacity == d.capacity);
    CHECK(s.rate == d.rate);
    CHECK(s.speed_min == d.speed_min);
    CHECK(s.speed_max == d.speed_max);
    CHECK(s.spread == doctest::Approx(d.spread));
    CHECK(s.life_min == d.life_min);
    CHECK(s.life_max == d.life_max);
    CHECK(s.size_start == d.size_start);
    CHECK(s.color_end == d.color_end);
    CHECK(s.layer == d.layer);
    CHECK_FALSE(static_cast<bool>(s.region.texture));
}

TEST_CASE("sanitize: hostile values become finite and bounded") {
    for (const float v : {kNaN, kInf, -kInf, 1.0e30f, -1.0e30f, -5.0f, 0.0f}) {
        CAPTURE(v);
        const EmitterConfig s = sanitize(hostile(v));
        CHECK(all_finite(s));
        CHECK(s.capacity >= 1);
        CHECK(s.capacity <= kMaxParticlesPerPool);
        CHECK(s.burst <= s.capacity);
        CHECK(s.rate >= 0.0f);
        CHECK(s.rate <= 1.0e6f);
        CHECK(s.speed_min <= s.speed_max);
        CHECK(s.speed_min >= 0.0f);
        CHECK(s.life_min >= 0.001f);
        CHECK(s.life_min <= s.life_max);
        CHECK(s.spread >= 0.0f);
        CHECK(s.spread <= doctest::Approx(kTau));
        CHECK(s.angle >= -kPi - 1e-4f);
        CHECK(s.angle <= kPi + 1e-4f);
        CHECK(s.size_start >= 0.0f);
        CHECK(s.size_end >= 0.0f);
        CHECK(s.drag >= 0.0f);
        CHECK(s.material < kMaxMaterials);
        CHECK(s.spawn_area.w >= 0.0f);
        CHECK(s.spawn_area.h >= 0.0f);
        CHECK(s.region.source.w >= 0.0f);
        CHECK(s.region.source.h >= 0.0f);
        CHECK(static_cast<int>(s.spawn_shape) <= static_cast<int>(SpawnShape::Ring));
        CHECK(s.rotation_min <= s.rotation_max);
        CHECK(s.spin_min <= s.spin_max);
        CHECK(s.prewarm <= 60.0f);
    }
}

TEST_CASE("sanitize: capacity is clamped to [1, 65536] and burst to capacity") {
    EmitterConfig c;
    c.capacity = 0;
    CHECK(sanitize(c).capacity == 1);
    c.capacity = 65536;
    CHECK(sanitize(c).capacity == 65536);
    c.capacity = 65537;
    CHECK(sanitize(c).capacity == 65536);
    c.capacity = 10;
    c.burst = 500;
    CHECK(sanitize(c).burst == 10);
}

TEST_CASE("sanitize: reversed ranges are ordered and negative areas flipped") {
    EmitterConfig c;
    c.speed_min = 50;
    c.speed_max = 10;
    c.life_min = 2;
    c.life_max = 1;
    c.spawn_area = {10, 10, -4, -6};
    const EmitterConfig s = sanitize(c);
    CHECK(s.speed_min == 10);
    CHECK(s.speed_max == 50);
    CHECK(s.life_min == 1);
    CHECK(s.life_max == 2);
    CHECK(s.spawn_area == Rect{6, 4, 4, 6});
}

TEST_CASE("sanitize: a textured region without a source uses its frame, else the circle") {
    EmitterConfig c;
    c.region.texture = AssetId<Texture>{5, 1};
    c.region.size = {8, 4};
    EmitterConfig s = sanitize(c);
    CHECK(s.region.texture == AssetId<Texture>{5, 1});
    CHECK(s.region.source == Rect{0, 0, 8, 4});
    c.region.size = {};
    s = sanitize(c);
    CHECK_FALSE(static_cast<bool>(s.region.texture)); // nothing to draw from: built-in circle
    // A built-in source on the shape page is kept as is.
    EmitterConfig square;
    square.region.source = kWhiteSource;
    CHECK(sanitize(square).region.source == kWhiteSource);
}

TEST_CASE("burst: adds to pending_burst and saturates") {
    ParticleEmitter e;
    burst(e, 10);
    burst(e, 5);
    CHECK(e.pending_burst == 15);
    burst(e, 65535);
    CHECK(e.pending_burst == 65535);
}

TEST_CASE("presets: intensity is clamped to [0, 1]; capacity is fixed") {
    for (const float bad : {kNaN, -kInf, -3.0f}) {
        CAPTURE(bad);
        CHECK(rain_preset(bad).rate == 0.0f);
        CHECK(snow_preset(bad).rate == 0.0f);
    }
    for (const float big : {kInf, 1.5f, 100.0f, 1.0e30f}) {
        CAPTURE(big);
        CHECK(rain_preset(big).rate == doctest::Approx(rain_preset(1.0f).rate));
        CHECK(snow_preset(big).rate == doctest::Approx(snow_preset(1.0f).rate));
    }
    CHECK(rain_preset(0.5f).rate == doctest::Approx(rain_preset(1.0f).rate * 0.5f));
    CHECK(rain_preset(0.0f).capacity == rain_preset(1.0f).capacity);
    CHECK(snow_preset(0.2f).capacity == snow_preset(1.0f).capacity);
    CHECK(is_screen_layer(rain_preset(1).layer));
    CHECK(is_screen_layer(snow_preset(1).layer));
    CHECK(all_finite(rain_preset(1)));
    CHECK(all_finite(snow_preset(1)));
    // Presets are already sanitized.
    CHECK(sanitize(rain_preset(1)).rate == rain_preset(1).rate);
    CHECK(sanitize(snow_preset(1)).capacity == snow_preset(1).capacity);
}

TEST_CASE("presets: other screen sizes widen the spawn band; bad sizes fall back") {
    const EmitterConfig wide = rain_preset(1, {960, 540});
    const EmitterConfig def = rain_preset(1);
    CHECK(wide.spawn_area.w > def.spawn_area.w);
    CHECK(wide.life_max > def.life_max); // taller screen, longer fall
    const EmitterConfig bad = rain_preset(1, {kNaN, -1});
    CHECK(bad.spawn_area == def.spawn_area);
    CHECK(snow_preset(1, {0, 0}).spawn_area == snow_preset(1).spawn_area);
}

// Rain is a fixed-capacity pool; intensity changes ease the drop count in and out.
TEST_CASE("rain eases in and out and keeps its drops in budget") {
    ParticlePools pools;
    Rng rng(3);
    const ParticlePoolId rain = pools.create(rain_preset(1.0f));
    REQUIRE(rain);
    const std::uint32_t budget = pools.capacity(rain);
    CHECK(budget == 256);
    const float dt = 1.0f / 60.0f;
    float accumulator = 0;
    const auto run = [&](const EmitterConfig &c, int frames, std::uint32_t *peak) {
        for (int i = 0; i < frames; ++i) {
            pools.update(dt, rng);
            accumulator += c.rate * dt;
            const auto n = static_cast<std::uint32_t>(accumulator);
            accumulator -= static_cast<float>(n);
            pools.emit_stream(rain, {}, {}, n, dt, rng);
            if (peak != nullptr && pools.alive(rain) > *peak) *peak = pools.alive(rain);
            CHECK(pools.alive(rain) <= budget);
        }
    };
    // Easing in: after 0.25 s the field is still filling.
    std::uint32_t peak = 0;
    run(rain_preset(1.0f), 15, &peak);
    const std::uint32_t early = pools.alive(rain);
    CHECK(early > 0);
    run(rain_preset(1.0f), 165, &peak); // 3 s in total: steady state
    const std::uint32_t full = pools.alive(rain);
    CHECK(early < full);
    CHECK(full == doctest::Approx(220).epsilon(0.15)); // about 220 drops at full intensity
    CHECK(peak <= budget);
    // Half intensity settles at about half the drops.
    run(rain_preset(0.5f), 180, nullptr);
    CHECK(pools.alive(rain) == doctest::Approx(110).epsilon(0.25));
    // Easing out: drops die off over their fall time, then the field is dry.
    run(rain_preset(0.0f), 20, nullptr);
    CHECK(pools.alive(rain) > 0);
    run(rain_preset(0.0f), 120, nullptr);
    CHECK(pools.alive(rain) == 0);
    // Hostile intensities never exceed the fixed budget either.
    const ParticlePoolId wild = pools.create(rain_preset(1.0e30f));
    REQUIRE(wild);
    for (int i = 0; i < 300; ++i) {
        pools.update(dt, rng);
        pools.emit_stream(wild, {}, {}, 1000, dt, rng);
    }
    CHECK(pools.alive(wild) <= pools.capacity(wild));
    CHECK(pools.capacity(wild) == 256);
}

TEST_CASE("snow keeps its flakes in budget and sways inside the screen band") {
    ParticlePools pools;
    Rng rng(9);
    const EmitterConfig snow = snow_preset(1.0f);
    const ParticlePoolId p = pools.create(snow);
    REQUIRE(p);
    float acc = 0;
    for (int i = 0; i < 60 * 20; ++i) {
        pools.update(1.0f / 60.0f, rng);
        acc += snow.rate / 60.0f;
        const auto n = static_cast<std::uint32_t>(acc);
        acc -= static_cast<float>(n);
        pools.emit_stream(p, {}, {}, n, 1.0f / 60.0f, rng);
    }
    CHECK(pools.alive(p) <= pools.capacity(p));
    CHECK(pools.alive(p) == doctest::Approx(300).epsilon(0.2));
    for (const ParticlePools::Particle &f : pools.particles(p)) {
        CHECK(std::isfinite(f.position.x));
        CHECK(f.position.y >= -20.0f); // flakes only fall
    }
}
