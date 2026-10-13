// Sort keys, tie-breaks and plan_batches. Y-sorted sprites sort by feet, then layer, then entity
// index.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <lam/render/passes.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>
#include <lam/test/support.h>

namespace {

using namespace lam;

constexpr AssetId<Texture> tex(std::uint32_t index) {
    return AssetId<Texture>{index, 1};
}

// A 1x1 quad whose colour's red channel identifies it.
void quad(RenderQueue &q, std::uint8_t id, std::uint8_t layer, float depth, AssetId<Texture> t = {},
          MaterialId material = 0, std::uint32_t tiebreak = 0) {
    const Color c{id, 0, 0, 255};
    const QuadVertex v[4] = {
        {{0, 0}, {0, 0}, c}, {{1, 0}, {1, 0}, c}, {{1, 1}, {1, 1}, c}, {{0, 1}, {0, 1}, c}};
    q.push(layer, depth, material, t, v, tiebreak);
}

struct Planned {
    SortScratch scratch;
    std::vector<Batch> batches;
    std::vector<QuadVertex> staging;
    void plan(const RenderQueue &q, const RenderPasses *p = nullptr) {
        plan_batches(q, p, scratch, batches, staging);
    }
    std::vector<int> order() const {
        std::vector<int> out;
        for (std::size_t i = 0; i < staging.size(); i += 4)
            out.push_back(staging[i].color.r);
        return out;
    }
};

CameraView view_at(Vec2 center) {
    Camera2D c;
    c.position = center;
    return make_camera_view(c, {480, 270}, {}, true);
}

} // namespace

TEST_CASE("keys: depth_bits quantises to 1/16 px, clamps, and maps NaN to 0") {
    CHECK(depth_bits(0.0f) == 524288u * 16u);
    CHECK(depth_bits(-1.0f / 16.0f) == 524288u * 16u - 1u);
    // Full 1/16 px resolution at and above depth 0 too (a float version rounded odd steps up
    // there, giving 1/8 px).
    CHECK(depth_bits(1.0f / 16.0f) == 524288u * 16u + 1u);
    CHECK(depth_bits(3.0f / 16.0f) == 524288u * 16u + 3u);
    CHECK(depth_bits(1.0f / 8.0f) == 524288u * 16u + 2u);
    for (int i = -64; i < 64; ++i)
        CHECK(depth_bits(static_cast<float>(i) / 16.0f) + 1u ==
              depth_bits(static_cast<float>(i + 1) / 16.0f));
    static_assert(depth_bits(1.0f / 16.0f) == 524288u * 16u + 1u); // still constexpr
    CHECK(depth_bits(-524288.0f) == 0u);
    CHECK(depth_bits(-1.0e9f) == 0u);
    CHECK(depth_bits(1.0e9f) == 0xFFFFFFu);
    CHECK(depth_bits(std::numeric_limits<float>::quiet_NaN()) == 0u);
    CHECK(depth_bits(100.25f) < depth_bits(100.5f));
    CHECK(depth_bits(-3.0f) < depth_bits(2.0f));
}

TEST_CASE("keys: make_sort_key packs view | layer | depth | material | texture | clip") {
    const std::uint64_t k = make_sort_key(5, 200, 0x123456, 17, 0xBEEF, 9);
    CHECK(key_view(k) == 5);
    CHECK(key_layer(k) == 200);
    CHECK(key_material(k) == 17);
    CHECK(key_texture(k) == 0xBEEF);
    CHECK(key_clip(k) == 9);
    // Field order: view outranks layer outranks depth outranks material outranks texture.
    CHECK(make_sort_key(1, 0, 0, 0, 0, 0) > make_sort_key(0, 255, 0xFFFFFF, 31, 0xFFFF, 255));
    CHECK(make_sort_key(0, 1, 0, 0, 0, 0) > make_sort_key(0, 0, 0xFFFFFF, 31, 0xFFFF, 255));
    CHECK(make_sort_key(0, 0, 1, 0, 0, 0) > make_sort_key(0, 0, 0, 31, 0xFFFF, 255));
    CHECK(make_sort_key(0, 0, 0, 1, 0, 0) > make_sort_key(0, 0, 0, 0, 0xFFFF, 255));
}

TEST_CASE("depth: items are sorted back to front by their feet") {
    RenderQueue q(64);
    quad(q, 0, kLayerWorld, 300.0f);
    quad(q, 1, kLayerWorld, 100.5f);
    quad(q, 2, kLayerWorld, -20.0f);
    quad(q, 3, kLayerWorld, 100.25f);
    Planned p;
    p.plan(q);
    CHECK(p.order() == std::vector<int>{2, 3, 1, 0});
}

TEST_CASE(
    "depth: at the same y lower layers draw first, then the tiebreak (entity index) decides") {
    RenderQueue q(64);
    quad(q, 1, kLayerWorld + 1, 64.0f, {}, 0, 8); // a canopy of entity 7 (top layer)
    quad(q, 2, kLayerWorld, 64.0f, {}, 0, 8);     // its trunk
    quad(q, 3, kLayerWorld, 64.0f, {}, 0, 10);    // entity 9
    quad(q, 4, kLayerWorld, 64.0f, {}, 0, 4);     // entity 3
    quad(q, 5, kLayerWorld + 1, 64.0f, {}, 0, 2); // canopy of entity 1
    Planned p;
    p.plan(q);
    CHECK(p.order() == std::vector<int>{4, 2, 3, 5, 1});
}

TEST_CASE("depth: equal y gives the same order whatever order the items arrive in") {
    struct Item {
        std::uint8_t id;
        std::uint8_t layer;
        float y;
        std::uint32_t tiebreak;
    };
    const std::array<Item, 5> expected{{{1, kLayerWorld, 90.0f, 4},
                                        {2, kLayerWorld, 120.0f, 1},
                                        {3, kLayerWorld, 120.0f, 5},
                                        {4, kLayerWorld, 120.0f, 7},
                                        {5, kLayerWorld + 1, 120.0f, 7}}};
    std::array<std::size_t, 5> order{0, 1, 2, 3, 4};
    int permutations = 0;
    RenderQueue q(64);
    Planned p;
    do {
        q.clear();
        for (std::size_t i : order)
            quad(q, expected[i].id, expected[i].layer, expected[i].y, {}, 0, expected[i].tiebreak);
        p.plan(q);
        CHECK(p.order() == std::vector<int>{1, 2, 3, 4, 5});
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));
    CHECK(permutations == 120);
}

TEST_CASE("depth: planning twice, or planning nothing, changes nothing") {
    RenderQueue q(64);
    quad(q, 1, kLayerWorld, 5.0f, {}, 0, 3);
    quad(q, 2, kLayerWorld, 5.0f, {}, 0, 2);
    quad(q, 3, kLayerWorld + 1, 4.0f, {}, 0, 1);
    Planned p;
    p.plan(q);
    const std::vector<int> once = p.order();
    CHECK(once == std::vector<int>{2, 1, 3});
    p.plan(q);
    CHECK(p.order() == once);
    RenderQueue empty(4);
    p.plan(empty);
    CHECK(p.batches.empty());
    CHECK(p.staging.empty());
}

TEST_CASE("depth: a moving character changes place in the order exactly when its feet pass") {
    RenderQueue q(8);
    Planned p;
    for (float y = 98.0f; y <= 102.0f; y += 0.25f) {
        q.clear();
        quad(q, 0, kLayerWorld, y, {}, 0, 1);      // the character (entity 0)
        quad(q, 1, kLayerWorld, 100.0f, {}, 0, 2); // a rock (entity 1)
        p.plan(q);
        CAPTURE(y);
        CHECK(p.order()[0] == (y <= 100.0f ? 0 : 1));
    }
}

TEST_CASE("radix sort: stable for equal (key, tiebreak); total over 96 bits; many items") {
    RenderQueue q(70000);
    // Equal keys and equal explicit tiebreaks keep submission order.
    for (int i = 0; i < 50; ++i)
        quad(q, static_cast<std::uint8_t>(i), kLayerWorld, 7.0f, {}, 0, 42);
    Planned p;
    p.plan(q);
    std::vector<int> expected(50);
    for (int i = 0; i < 50; ++i)
        expected[static_cast<std::size_t>(i)] = i;
    CHECK(p.order() == expected);

    // A pseudo-random set against std::stable_sort on (key, tiebreak); each quad's identity is
    // its first vertex's x.
    q.clear();
    std::uint32_t s = 99;
    for (int i = 0; i < 60000; ++i) {
        s = s * 1664525u + 1013904223u;
        const float depth = static_cast<float>(static_cast<int>(s >> 8) % 4000 - 2000) * 0.25f;
        const std::uint8_t layer = static_cast<std::uint8_t>(kLayerTiles + (s >> 28) % 3);
        const QuadVertex v[4] = {{{static_cast<float>(i), 0}, {}, kWhite}, {}, {}, {}};
        q.push(layer, depth, static_cast<MaterialId>((s >> 12) & 1), tex(1 + ((s >> 4) & 3)), v,
               (s >> 3) & 0x3FFF);
    }
    p.plan(q);
    std::vector<std::size_t> idx(q.size());
    for (std::size_t i = 0; i < idx.size(); ++i)
        idx[i] = i;
    const auto quads = q.quads();
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
        if (quads[a].key != quads[b].key) return quads[a].key < quads[b].key;
        return quads[a].tiebreak < quads[b].tiebreak;
    });
    bool same = p.staging.size() == q.size() * 4;
    for (std::size_t k = 0; k < idx.size() && same; ++k)
        same = p.staging[k * 4].pos.x == static_cast<float>(idx[k]);
    CHECK(same);
    std::uint32_t total = 0;
    for (const Batch &b : p.batches)
        total += b.quad_count;
    CHECK(total == q.size());
}

TEST_CASE(
    "radix sort: tiebreaks already ordered per layer skip their passes and give the same order") {
    // Tiebreaks non-decreasing within each layer (the common case: sequence numbers, ECS sprites
    // in entity order), with wide values and duplicates; then one out-of-order tiebreak, which
    // must fall back to the full sort. Both against std::stable_sort on (key, tiebreak).
    for (const bool disorder : {false, true}) {
        CAPTURE(disorder);
        RenderQueue q(40000);
        std::uint32_t s = 7;
        std::array<std::uint32_t, 3> next{1, 1, 1};
        for (int i = 0; i < 30000; ++i) {
            s = s * 1664525u + 1013904223u;
            const std::size_t l = (s >> 28) % 3;
            const float depth = static_cast<float>(static_cast<int>(s >> 8) % 64);
            next[l] += (s >> 5) % 3 == 0 ? 0u : 70000u; // duplicates and > 16-bit values
            std::uint32_t tb = next[l];
            if (disorder && i == 20000) tb = 1;
            const QuadVertex v[4] = {{{static_cast<float>(i), 0}, {}, kWhite}, {}, {}, {}};
            q.push(static_cast<std::uint8_t>(kLayerTiles + l), depth, 0, tex(1 + ((s >> 3) & 1)), v,
                   tb);
        }
        Planned p;
        p.plan(q);
        std::vector<std::size_t> idx(q.size());
        for (std::size_t i = 0; i < idx.size(); ++i)
            idx[i] = i;
        const auto quads = q.quads();
        std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
            if (quads[a].key != quads[b].key) return quads[a].key < quads[b].key;
            return quads[a].tiebreak < quads[b].tiebreak;
        });
        bool same = p.staging.size() == q.size() * 4;
        for (std::size_t k = 0; k < idx.size() && same; ++k)
            same = p.staging[k * 4].pos.x == static_cast<float>(idx[k]);
        CHECK(same);
    }
}

TEST_CASE("plan_batches: layers alone do not break a batch; texture, material, clip and space do") {
    RenderQueue q(64);
    q.begin_view(view_at({240, 135}));
    quad(q, 0, kLayerTiles, 0, tex(3));
    quad(q, 1, kLayerTiles + 1, 0, tex(3));
    quad(q, 2, kLayerWorld, 10, tex(3));
    quad(q, 3, kLayerForeground, 0, tex(3));
    Planned p;
    p.plan(q);
    REQUIRE(p.batches.size() == 1);
    CHECK(p.batches[0].quad_count == 4);
    CHECK(p.batches[0].texture == 3);
    CHECK_FALSE(p.batches[0].screen_space);

    quad(q, 4, kLayerForeground, 1, tex(4));                    // texture
    quad(q, 5, kLayerForeground, 2, tex(4), kMaterialAdditive); // material
    q.set_clip(q.clip({0, 0, 10, 10}));
    quad(q, 6, kLayerForeground, 3, tex(4), kMaterialAdditive); // clip
    q.set_clip(0);
    quad(q, 7, kLayerScreenFx, 0, tex(4), kMaterialAdditive); // screen space
    p.plan(q);
    REQUIRE(p.batches.size() == 5);
    CHECK(p.batches[1].texture == 4);
    CHECK(p.batches[2].material == kMaterialAdditive);
    CHECK(p.batches[3].clip == 1);
    CHECK(p.batches[4].screen_space);
    CHECK(p.order() == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7});
}

TEST_CASE("plan_batches: views draw bottom first and break batches") {
    RenderQueue q(64);
    const std::uint8_t a = q.begin_view(view_at({0, 0}));
    quad(q, 0, kLayerUi, 0, tex(2));
    const std::uint8_t b = q.begin_view(view_at({100, 100}));
    quad(q, 1, kLayerWorld, 0, tex(2));
    q.set_view(a);
    quad(q, 2, kLayerWorld, 5, tex(2));
    q.set_view(b);
    quad(q, 3, kLayerBackground, 0, tex(2));
    Planned p;
    p.plan(q);
    CHECK(p.order() == std::vector<int>{2, 0, 3, 1});
    REQUIRE(p.batches.size() == 3);
    CHECK(p.batches[0].view == a);
    CHECK(p.batches[1].view == a);
    CHECK(p.batches[1].screen_space);
    CHECK(p.batches[2].view == b);
    CHECK(p.batches[2].quad_count == 2);
}

TEST_CASE("plan_batches: screen layers keep painter's order across textures") {
    RenderQueue q(64);
    quad(q, 0, kLayerUi, 99, tex(1));
    quad(q, 1, kLayerUi, -5, tex(2));
    quad(q, 2, kLayerUi, 0, tex(1));
    quad(q, 3, kLayerUi, 0, tex(1));
    Planned p;
    p.plan(q);
    CHECK(p.order() == std::vector<int>{0, 1, 2, 3});
    REQUIRE(p.batches.size() == 3);
    CHECK(p.batches[2].quad_count == 2);
}

TEST_CASE("plan_batches: a batch never holds more than kMaxQuadsPerBatch quads") {
    RenderQueue q(40000);
    for (std::uint32_t i = 0; i < kMaxQuadsPerBatch * 2 + 10; ++i)
        quad(q, 1, kLayerWorld, static_cast<float>(i % 300), tex(1));
    Planned p;
    p.plan(q);
    REQUIRE(p.batches.size() == 3);
    CHECK(p.batches[0].quad_count == kMaxQuadsPerBatch);
    CHECK(p.batches[1].quad_count == kMaxQuadsPerBatch);
    CHECK(p.batches[2].quad_count == 10);
    CHECK(p.batches[1].first_quad == kMaxQuadsPerBatch);
    CHECK(p.staging.size() == q.size() * 4);
}

TEST_CASE("plan_batches: routed layers go to their target; composites appear only where used") {
    RenderPasses passes;
    RenderTargetDesc d;
    d.virtual_scale = 0.5f;
    const RenderTargetId light = passes.add_target(str_id("light"), d);
    CHECK(light == 1);
    CHECK(passes.find(str_id("light")) == light);
    CHECK(passes.find(str_id("nope")) == 0);
    passes.route_layers(kLayerLights, kLayerLights + 8, light);
    passes.composite(light, kMaterialMultiply, kLayerLights + 9);
    CHECK(passes.target_of(kLayerLights + 3) == light);
    CHECK(passes.target_of(kLayerLights + 9) == 0);
    CHECK(passes.target_size(light, {480, 270}) == IVec2{240, 135});

    RenderQueue q(64);
    const std::uint8_t v0 = q.begin_view(view_at({0, 0}));
    quad(q, 0, kLayerWorld, 0, tex(1));
    quad(q, 1, kLayerLights, 0, tex(1), kMaterialAdditive);
    quad(q, 2, kLayerUi, 0, tex(1));
    const std::uint8_t v1 = q.begin_view(view_at({0, 0}));
    quad(q, 3, kLayerWorld, 0, tex(1)); // no lights in this view: no composite
    Planned p;
    p.plan(q, &passes);
    REQUIRE(p.batches.size() == 5);
    CHECK(p.batches[0].target == 0);
    CHECK(p.batches[1].target == light);
    CHECK(p.batches[2].composite_source == light);
    CHECK(p.batches[2].material == kMaterialMultiply);
    CHECK(p.batches[2].target == 0);
    CHECK(p.batches[2].view == v0);
    CHECK(p.batches[2].quad_count == 0);
    CHECK(p.batches[3].screen_space);
    CHECK(p.batches[4].view == v1);

    SUBCASE("a composite above every quad of its view is emitted at the view's end") {
        RenderQueue q2(8);
        q2.begin_view(view_at({0, 0}));
        quad(q2, 0, kLayerLights, 0, tex(1), kMaterialAdditive);
        p.plan(q2, &passes);
        REQUIRE(p.batches.size() == 2);
        CHECK(p.batches[1].composite_source == light);
    }
}

TEST_CASE("plan_batches: the bench scene plans to 10 or fewer draws, without allocating") {
    // 3 tile layers (560 quads each), y-sorted sprites, additive particles and a UI panel with
    // text, all from one atlas page (slot 1), as in the render bench scene.
    for (const int sprites : {10000, 50000}) {
        CAPTURE(sprites);
        RenderQueue q(70000);
        Planned p;
        const auto build = [&] {
            q.clear();
            q.begin_view(view_at({240, 135}));
            for (int layer = 0; layer < 3; ++layer)
                for (int i = 0; i < 560; ++i)
                    quad(q, 1, static_cast<std::uint8_t>(kLayerTiles + layer),
                         static_cast<float>(i % 17), tex(1));
            for (int i = 0; i < sprites; ++i)
                quad(q, 2, kLayerWorld, static_cast<float>((i * 7919) % 270), tex(1), 0,
                     static_cast<std::uint32_t>(i + 1));
            for (int i = 0; i < 400; ++i)
                quad(q, 3, kLayerForeground, 0, tex(1), kMaterialAdditive);
            for (int i = 0; i < 120; ++i)
                quad(q, 4, kLayerUi, 0, tex(1));
        };
        build();
        p.plan(q);
        CHECK(p.batches.size() <= 10);
        CHECK(p.batches.size() == (sprites == 10000 ? 3u : 6u));
        build();
        CHECK_NO_ALLOC(p.plan(q));
    }
}
