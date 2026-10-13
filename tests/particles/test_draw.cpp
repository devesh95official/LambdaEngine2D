// ParticlePools::draw: one quad per visible particle, with culling for world and screen layers.
#include <cmath>
#include <cstdint>
#include <vector>

#include <lam/math/rng.h>
#include <lam/particles/particles.h>
#include <lam/render/render_queue.h>
#include <lam/test/support.h>

#include "particles_test_util.h"

using namespace lam;
using lam::test::steady_config;

namespace {

constexpr AssetId<Texture> kShapePage{3, 1};

Rect quad_bounds(const DrawQuad &q) {
    Vec2 lo = q.v[0].pos, hi = q.v[0].pos;
    for (const QuadVertex &v : q.v) {
        lo = min(lo, v.pos);
        hi = max(hi, v.pos);
    }
    return Rect::from_min_max(lo, hi);
}

EmitterConfig still(std::uint32_t capacity = 16) {
    EmitterConfig c = steady_config(capacity, 0, 1.0f);
    c.speed_min = c.speed_max = 0;
    return c;
}

} // namespace

TEST_CASE("draw: one circle quad per particle on the shape page, centred, size over life") {
    ParticlePools pools;
    Rng rng(1);
    RenderQueue q(64);
    q.set_shape_texture(kShapePage);
    EmitterConfig c = still();
    c.size_start = 2;
    c.size_end = 0;
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {10, 20}, 3, rng);
    pools.update(0.5f, rng); // half way from 2 to 0
    pools.draw(q, Rect{0, 0, 100, 100});
    REQUIRE(q.size() == 3);
    CHECK(pools.stats().drawn == 3);
    const DrawQuad &d = q.quads()[0];
    CHECK(key_texture(d.key) == texture_slot(kShapePage));
    CHECK(key_layer(d.key) == kLayerForeground);
    CHECK(key_material(d.key) == 0);
    const Rect b = quad_bounds(d);
    CHECK(b.w == doctest::Approx(1.0f));
    CHECK(b.h == doctest::Approx(1.0f));
    CHECK(b.center().x == doctest::Approx(10.0f));
    CHECK(b.center().y == doctest::Approx(20.0f));
    // UVs cover the built-in circle, corners in tl, tr, br, bl order.
    CHECK(d.v[0].uv == Vec2{kCircleSource.x, kCircleSource.y});
    CHECK(d.v[2].uv == Vec2{kCircleSource.x + kCircleSource.w, kCircleSource.y + kCircleSource.h});
    CHECK(d.v[0].pos.x < d.v[1].pos.x);
    CHECK(d.v[1].pos.y < d.v[2].pos.y);
}

TEST_CASE("draw: colour over life; transparent and zero-size particles push nothing") {
    ParticlePools pools;
    Rng rng(1);
    RenderQueue q(64);
    EmitterConfig c = still();
    c.color_start = Color{255, 0, 0, 255};
    c.color_end = Color{0, 0, 255, 0};
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {}, 1, rng);
    pools.update(0.25f, rng);
    pools.draw(q, Rect{-50, -50, 100, 100});
    REQUIRE(q.size() == 1);
    const Color col = q.quads()[0].v[0].color;
    CHECK(col.r == 191);
    CHECK(col.b == 64);
    CHECK(col.a == 191);
    for (const QuadVertex &v : q.quads()[0].v)
        CHECK(v.color == col);
    // Fully transparent at the end of life: not drawn.
    q.clear();
    c.color_start = c.color_end = kTransparent;
    REQUIRE(pools.set_config(p, c));
    pools.draw(q, Rect{-50, -50, 100, 100});
    CHECK(q.size() == 0);
    // Zero size: not drawn.
    c.color_start = c.color_end = kWhite;
    c.size_start = c.size_end = 0;
    REQUIRE(pools.set_config(p, c));
    pools.draw(q, Rect{-50, -50, 100, 100});
    CHECK(q.size() == 0);
}

TEST_CASE("draw: textured regions keep their aspect; built-in sources use the shape page") {
    ParticlePools pools;
    Rng rng(1);
    RenderQueue q(64);
    q.set_shape_texture(kShapePage);
    EmitterConfig c = still();
    c.size_start = c.size_end = 8;
    c.region.texture = AssetId<Texture>{7, 2};
    c.region.source = {32, 16, 16, 8}; // 2:1
    c.material = 3;
    c.layer = kLayerWorld;
    c.depth = 12;
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {0, 0}, 1, rng);
    pools.draw(q, Rect{-50, -50, 100, 100});
    REQUIRE(q.size() == 1);
    const DrawQuad &d = q.quads()[0];
    CHECK(key_texture(d.key) == 7);
    CHECK(key_material(d.key) == 3);
    CHECK(key_layer(d.key) == kLayerWorld);
    CHECK(((d.key >> 29) & 0xFFFFFFu) == depth_bits(12));
    const Rect b = quad_bounds(d);
    CHECK(b.w == doctest::Approx(8.0f)); // the longer side equals the particle size
    CHECK(b.h == doctest::Approx(4.0f));
    CHECK(d.v[0].uv == Vec2{32, 16});
    CHECK(d.v[2].uv == Vec2{48, 24});
    // A square from the built-in block.
    q.clear();
    EmitterConfig sq = still();
    sq.region.source = kWhiteSource;
    const ParticlePoolId s = pools.create(sq);
    pools.emit(s, {}, 1, rng);
    pools.clear(p);
    pools.draw(q, Rect{-50, -50, 100, 100});
    REQUIRE(q.size() == 1);
    CHECK(key_texture(q.quads()[0].key) == texture_slot(kShapePage));
    CHECK(q.quads()[0].v[0].uv == Vec2{kWhiteSource.x, kWhiteSource.y});
}

TEST_CASE("draw: y_sort uses the particle's y as depth") {
    ParticlePools pools;
    Rng rng(1);
    RenderQueue q(64);
    EmitterConfig c = still();
    c.layer = kLayerWorld;
    c.y_sort = true;
    c.depth = 2;
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {5, 40}, 1, rng);
    pools.draw(q, Rect{-100, -100, 200, 200});
    REQUIRE(q.size() == 1);
    CHECK(((q.quads()[0].key >> 29) & 0xFFFFFFu) == depth_bits(42));
}

TEST_CASE("draw: rotation turns the quad; stretch aligns it with the velocity") {
    ParticlePools pools;
    Rng rng(1);
    RenderQueue q(64);
    SUBCASE("rotation") {
        EmitterConfig c = still();
        c.size_start = c.size_end = 4;
        c.rotation_min = c.rotation_max = kPi * 0.25f;
        const ParticlePoolId p = pools.create(c);
        pools.emit(p, {}, 1, rng);
        pools.draw(q, Rect{-50, -50, 100, 100});
        REQUIRE(q.size() == 1);
        const Rect b = quad_bounds(q.quads()[0]);
        CHECK(b.w == doctest::Approx(4.0f * std::sqrt(2.0f)).epsilon(1e-3));
    }
    SUBCASE("stretch") {
        EmitterConfig c = steady_config(4, 0, 10);
        c.size_start = c.size_end = 1;
        c.speed_min = c.speed_max = 100;
        c.angle = kPi * 0.5f; // straight down
        c.spread = 0;
        c.stretch = 0.05f;    // +5 px along the motion
        const ParticlePoolId p = pools.create(c);
        pools.emit(p, {0, 0}, 1, rng);
        pools.draw(q, Rect{-50, -50, 100, 100});
        REQUIRE(q.size() == 1);
        const Rect b = quad_bounds(q.quads()[0]);
        CHECK(b.h == doctest::Approx(6.0f).epsilon(1e-3)); // 1 + 100 * 0.05
        CHECK(b.w == doctest::Approx(1.0f).epsilon(1e-3));
    }
    SUBCASE("align without stretch keeps the size") {
        EmitterConfig c = steady_config(4, 0, 10);
        c.size_start = c.size_end = 2;
        c.region.source = {0, 0, 4, 2}; // 2:1, long side along the motion
        c.region.texture = AssetId<Texture>{9, 1};
        c.angle = kPi * 0.5f;
        c.spread = 0;
        c.align_to_velocity = true;
        const ParticlePoolId p = pools.create(c);
        pools.emit(p, {0, 0}, 1, rng);
        pools.draw(q, Rect{-50, -50, 100, 100});
        REQUIRE(q.size() == 1);
        const Rect b = quad_bounds(q.quads()[0]);
        CHECK(b.h == doctest::Approx(2.0f).epsilon(1e-3));
        CHECK(b.w == doctest::Approx(1.0f).epsilon(1e-3));
    }
}

TEST_CASE("draw: world layers are culled against the visible rect, per pool and per particle") {
    ParticlePools pools;
    Rng rng(1);
    RenderQueue q(256);
    EmitterConfig c = still(64);
    c.spawn_area = {0, 0, 200, 0}; // a 200 px line
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {0, 0}, 64, rng);
    pools.update(0.01f, rng); // bounds
    pools.draw(q, Rect{0, -10, 100, 20});
    const auto st = pools.stats();
    CHECK(st.drawn + st.culled == 64);
    CHECK(st.drawn > 16);
    CHECK(st.culled > 16);
    for (const DrawQuad &d : q.quads())
        CHECK(quad_bounds(d).overlaps(Rect{0, -10, 100, 20}));
    // A pool entirely outside the view is skipped as a whole.
    q.clear();
    pools.draw(q, Rect{1000, 1000, 50, 50});
    CHECK(q.size() == 0);
    CHECK(pools.stats().culled == 64);
}

TEST_CASE("draw: screen layers ignore the world view and use the screen rect") {
    ParticlePools pools;
    Rng rng(1);
    RenderQueue q(64);
    EmitterConfig c = still();
    c.layer = kLayerScreenFx;
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {100, 100}, 2, rng);
    pools.draw(q, Rect{5000, 5000, 10, 10}); // world view elsewhere: screen pools still draw
    CHECK(q.size() == 2);
    q.clear();
    pools.draw(q, Rect{5000, 5000, 10, 10}, Rect{0, 0, 480, 270});
    CHECK(q.size() == 2);
    q.clear();
    pools.draw(q, Rect{0, 0, 480, 270}, Rect{0, 0, 50, 50});
    CHECK(q.size() == 0);
    CHECK(pools.stats().culled == 2);
}

TEST_CASE("draw: local-space pools draw relative to the anchor") {
    ParticlePools pools;
    Rng rng(1);
    RenderQueue q(64);
    EmitterConfig c = still();
    c.local_space = true;
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {10, 10}, 1, rng);
    pools.set_origin(p, {300, 40});
    pools.draw(q, Rect{0, 0, 1000, 1000});
    REQUIRE(q.size() == 1);
    CHECK(quad_bounds(q.quads()[0]).center().x == doctest::Approx(300.0f));
    CHECK(quad_bounds(q.quads()[0]).center().y == doctest::Approx(40.0f));
}

TEST_CASE("draw: sway offsets snow sideways within its amplitude") {
    ParticlePools pools;
    Rng rng(1);
    RenderQueue q(256);
    EmitterConfig c = still(100);
    c.sway = 6;
    c.sway_hz = 0.5f;
    c.life_min = c.life_max = 10;
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {50, 50}, 100, rng);
    pools.update(0.3f, rng);
    pools.draw(q, Rect{0, 0, 100, 100});
    REQUIRE(q.size() == 100);
    float lo = 1e9f, hi = -1e9f;
    for (const DrawQuad &d : q.quads()) {
        const float x = quad_bounds(d).center().x;
        lo = std::min(lo, x);
        hi = std::max(hi, x);
        CHECK(std::fabs(x - 50.0f) <= 6.0f + 1e-3f);
    }
    CHECK(hi - lo > 4.0f); // random phases
}

TEST_CASE("draw: no allocation below the queue capacity") {
    ParticlePools pools;
    Rng rng(1);
    RenderQueue q(8192);
    EmitterConfig c = steady_config(4096, 0, 100);
    c.rotation_max = 3;
    const ParticlePoolId p = pools.create(c);
    pools.emit(p, {}, 4096, rng);
    pools.draw(q, Rect{-1000, -1000, 2000, 2000});
    q.clear();
    CHECK_NO_ALLOC(for (int i = 0; i < 50; ++i) {
        pools.update(1.0f / 60.0f, rng);
        pools.draw(q, Rect{-1000, -1000, 2000, 2000});
        q.clear();
    });
}

// Layers don't break batches (see render_queue.h), so two pools on the shape page plan into one
// batch, in layer order.
TEST_CASE("draw: particles of one texture plan into one batch, after sprites of the layer below") {
    ParticlePools pools;
    Rng rng(1);
    RenderQueue q(8192);
    q.set_shape_texture(kShapePage);
    q.begin_view(CameraView{});
    const ParticlePoolId a = pools.create(still(2000));
    EmitterConfig other = still(2000);
    other.layer = kLayerWorld;
    other.y_sort = true;
    const ParticlePoolId b = pools.create(other);
    pools.emit(a, {}, 2000, rng);
    pools.emit(b, {20, 0}, 2000, rng);
    pools.draw(q, Rect{-100, -100, 200, 200});
    REQUIRE(q.size() == 4000);
    SortScratch scratch;
    std::vector<Batch> batches;
    std::vector<QuadVertex> staging;
    plan_batches(q, nullptr, scratch, batches, staging);
    REQUIRE(batches.size() == 1);
    CHECK(batches[0].quad_count == 4000);
    CHECK(batches[0].texture == texture_slot(kShapePage));
    REQUIRE(staging.size() == 4000u * 4u);
    const auto center_x = [&](std::size_t quad) {
        return (staging[quad * 4].pos.x + staging[quad * 4 + 2].pos.x) * 0.5f;
    };
    CHECK(center_x(0) == doctest::Approx(20.0f)); // pool b (kLayerWorld) first
    CHECK(center_x(1999) == doctest::Approx(20.0f));
    CHECK(center_x(2000) == doctest::Approx(0.0f)); // then pool a (kLayerForeground)
    CHECK(center_x(3999) == doctest::Approx(0.0f));
}
