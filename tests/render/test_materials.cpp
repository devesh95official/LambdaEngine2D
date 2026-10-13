// Shaders, materials, passes and lighting.
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include <lam/assets/asset_server.h>
#include <lam/render/lighting.h>
#include <lam/render/passes.h>
#include <lam/render/render_plugin.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>

#include "detail/render_internal.h"

namespace {

using namespace lam;

constexpr ShaderDialect kDialects[] = {ShaderDialect::Glsl330, ShaderDialect::GlslEs300,
                                       ShaderDialect::GlslEs100};
constexpr ShaderStage kStages[] = {ShaderStage::Vertex, ShaderStage::Fragment};

bool contains(std::string_view s, std::string_view part) {
    return s.find(part) != std::string_view::npos;
}

} // namespace

TEST_CASE("shader source: header, #line 1, then the body, for every dialect and stage") {
    const std::string_view body = "void main() { FRAG_COLOR = vec4(1.0); }\n";
    for (ShaderDialect d : kDialects) {
        for (ShaderStage s : kStages) {
            Result<std::string> src = build_shader_source(d, s, body, "a.fs");
            REQUIRE(src);
            const std::string_view header = shader_header(d, s);
            CHECK(src->substr(0, header.size()) == header);
            CHECK(src->substr(header.size(), 8) == "#line 1\n");
            CHECK(src->substr(header.size() + 8) == body);
            CHECK(header.back() == '\n');
            // Exactly one #version, on the first line.
            CHECK(src->rfind("#version", 0) == 0);
            CHECK(src->find("#version", 1) == std::string::npos);
        }
    }
    CHECK(contains(shader_header(ShaderDialect::Glsl330, ShaderStage::Vertex), "#version 330\n"));
    CHECK(contains(shader_header(ShaderDialect::GlslEs300, ShaderStage::Fragment),
                   "#version 300 es\n"));
    CHECK(contains(shader_header(ShaderDialect::GlslEs100, ShaderStage::Vertex), "#version 100\n"));
}

TEST_CASE("shader source: the dialect macros map to each language's keywords") {
    const auto h = [](ShaderDialect d, ShaderStage s) { return shader_header(d, s); };
    CHECK(contains(h(ShaderDialect::Glsl330, ShaderStage::Vertex), "#define ATTRIBUTE in\n"));
    CHECK(contains(h(ShaderDialect::Glsl330, ShaderStage::Vertex), "#define VARYING out\n"));
    CHECK(contains(h(ShaderDialect::Glsl330, ShaderStage::Fragment), "#define VARYING in\n"));
    CHECK(
        contains(h(ShaderDialect::Glsl330, ShaderStage::Fragment), "#define TEXTURE2D texture\n"));
    CHECK(contains(h(ShaderDialect::Glsl330, ShaderStage::Fragment), "out vec4 finalColor;"));
    CHECK(contains(h(ShaderDialect::GlslEs100, ShaderStage::Vertex),
                   "#define ATTRIBUTE attribute\n"));
    CHECK(contains(h(ShaderDialect::GlslEs100, ShaderStage::Vertex), "#define VARYING varying\n"));
    CHECK(contains(h(ShaderDialect::GlslEs100, ShaderStage::Fragment),
                   "#define TEXTURE2D texture2D\n"));
    CHECK(contains(h(ShaderDialect::GlslEs100, ShaderStage::Fragment),
                   "#define FRAG_COLOR gl_FragColor\n"));
    // ES fragment stages declare a float precision (highp where available); desktop needs none.
    CHECK(contains(h(ShaderDialect::GlslEs300, ShaderStage::Fragment), "precision highp float;"));
    CHECK(contains(h(ShaderDialect::GlslEs100, ShaderStage::Fragment), "precision highp float;"));
    CHECK(contains(h(ShaderDialect::GlslEs100, ShaderStage::Fragment), "precision mediump float;"));
    CHECK_FALSE(contains(h(ShaderDialect::Glsl330, ShaderStage::Fragment), "precision"));
    // A second colour output where the dialect has MRT; on ES 1.00 FRAG_OUTPUT1 is a plain vec4.
    CHECK(contains(h(ShaderDialect::Glsl330, ShaderStage::Fragment), "#define SHADER_HAS_MRT 1"));
    CHECK(contains(h(ShaderDialect::GlslEs300, ShaderStage::Fragment),
                   "layout(location = 1) out vec4"));
    CHECK_FALSE(contains(h(ShaderDialect::GlslEs100, ShaderStage::Fragment), "SHADER_HAS_MRT"));
    // Stage markers.
    CHECK(contains(h(ShaderDialect::GlslEs300, ShaderStage::Vertex), "#define SHADER_VERTEX 1"));
    CHECK(
        contains(h(ShaderDialect::GlslEs300, ShaderStage::Fragment), "#define SHADER_FRAGMENT 1"));
}

TEST_CASE(
    "shader source: bodies with their own #version or #extension, or a BOM, are rejected by name") {
    const auto rejected = [](std::string_view body, std::string_view why) {
        Result<std::string> r = build_shader_source(ShaderDialect::Glsl330, ShaderStage::Fragment,
                                                    body, "shaders/glow.fs");
        if (r) return false;
        CAPTURE(r.error().message);
        return r.error().code == ErrorCode::Parse &&
               contains(r.error().message, "shaders/glow.fs") && contains(r.error().message, why);
    };
    CHECK(rejected("#version 330\nvoid main() {}\n", "#version"));
    CHECK(rejected("  \t# version 100\n", "#version"));
    CHECK(rejected("void main() {}\n#extension GL_OES_standard_derivatives : enable\n",
                   "#extension"));
    CHECK(rejected("\xEF\xBB\xBFvoid main() {}\n", "byte-order mark"));
    // Mentions that are not directives are fine.
    for (std::string_view ok : {"// needs #version 330\nvoid main() {}\n",
                                "void main() {} // #extension\n", "#define VERSION 3\n", ""}) {
        CAPTURE(ok);
        CHECK(build_shader_source(ShaderDialect::Glsl330, ShaderStage::Fragment, ok, "ok.fs"));
    }
}

TEST_CASE("shader source: the engine shaders use the uniforms the batcher sets") {
    const std::string_view vs = engine_vertex_shader_body();
    for (std::string_view u : {"uniform mat4 mvp", "u_view_x", "u_view_y", "u_snap",
                               "u_inv_texture_size", "ATTRIBUTE vec2 vertexPosition",
                               "ATTRIBUTE vec2 vertexTexCoord", "ATTRIBUTE vec4 vertexColor"}) {
        CAPTURE(u);
        CHECK(contains(vs, u));
    }
    CHECK(contains(engine_fragment_shader_body(), "texture0"));
    CHECK(contains(upscale_fragment_shader_body(), "u_source_size"));
    CHECK(contains(upscale_fragment_shader_body(), "u_scale"));
    for (ShaderDialect d : kDialects) {
        CHECK(build_shader_source(d, ShaderStage::Vertex, engine_vertex_shader_body(), "engine"));
        CHECK(
            build_shader_source(d, ShaderStage::Fragment, engine_fragment_shader_body(), "engine"));
        CHECK(build_shader_source(d, ShaderStage::Fragment, upscale_fragment_shader_body(),
                                  "engine"));
    }
}

TEST_CASE("shader json: either stage may be omitted; unknown keys are errors") {
    Result<Shader> s = detail::parse_shader_json(R"({"fs": "glow.fs"})", "glow.shader.json");
    REQUIRE(s);
    CHECK(s->vs_path.empty());
    CHECK(s->fs_path == "glow.fs");
    CHECK(s->gpu_id == 0u);
    Result<Shader> both =
        detail::parse_shader_json(R"({"vs": "a.vs", "fs": "a.fs"})", "a.shader.json");
    REQUIRE(both);
    CHECK(both->vs_path == "a.vs");
    CHECK_FALSE(detail::parse_shader_json(R"({"vs": 3})", "bad.shader.json"));
    CHECK_FALSE(detail::parse_shader_json(R"({"fragment": "a.fs"})", "bad.shader.json"));
    CHECK_FALSE(detail::parse_shader_json("{", "bad.shader.json"));
}

TEST_CASE("shader json: loads headless as a placeholder (gpu_id 0: the engine shader draws it)") {
    test::TempDir dir;
    test::write_test_file(dir, "shaders/glow.shader.json", R"({"fs": "glow.fs"})");
    AssetsConfig cfg;
    cfg.root = dir.path();
    test::HeadlessApp h;
    h.add<AssetsPlugin>(cfg);
    h.add<RenderPlugin>();
    Result<AssetId<Shader>> id =
        h.app().res<AssetServer>().load_now<Shader>("shaders/glow.shader.json");
    REQUIRE(id);
    const Shader *s = h.app().res<AssetServer>().get<Shader>(id.value());
    REQUIRE(s != nullptr);
    CHECK(s->gpu_id == 0u);
    CHECK(s->fs_path == "glow.fs");
    Materials &m = h.app().res<Materials>();
    const MaterialId glow = m.add(Material{id.value(), BlendMode::Additive});
    CHECK(glow == 3);
    CHECK(m.get(glow).shader == id.value());
}

TEST_CASE("materials: alpha, additive and multiply are pre-registered; add appends") {
    Materials m;
    REQUIRE(m.size() == 3u);
    CHECK(m.get(kMaterialAlpha).blend == BlendMode::Alpha);
    CHECK(m.get(kMaterialAdditive).blend == BlendMode::Additive);
    CHECK(m.get(kMaterialMultiply).blend == BlendMode::Multiply);
    CHECK_FALSE(m.get(kMaterialAlpha).shader);
    for (int i = 3; i < kMaxMaterials; ++i)
        CHECK(m.add(Material{{}, BlendMode::Premultiplied}) == i);
    CHECK(m.size() == kMaxMaterials);
    CHECK(m.get(31).blend == BlendMode::Premultiplied);
    std::size_t n = 0;
    CHECK_NO_ALLOC(n = Materials().size());
    CHECK(n == 3u);
}

TEST_CASE("passes: targets, routes and composites") {
    RenderPasses p;
    CHECK(p.target_count() == 0u);
    CHECK(p.target_of(100) == 0);
    const std::uint32_t v0 = p.version();
    RenderTargetDesc d;
    d.virtual_scale = 0.5f;
    const RenderTargetId a = p.add_target(str_id("a"), d);
    CHECK(a == 1);
    CHECK(p.version() > v0);
    CHECK(p.find(str_id("a")) == a);
    CHECK(p.find(str_id("b")) == 0);
    CHECK(p.find(StrId{}) == 0);
    CHECK(p.name_of(a) == str_id("a"));
    CHECK(p.name_of(0) == StrId{});
    // A known name replaces the description and keeps the id.
    d.virtual_scale = 0.25f;
    CHECK(p.add_target(str_id("a"), d) == a);
    CHECK(p.desc(a).virtual_scale == 0.25f);
    CHECK(p.target_size(a, {480, 270}) == IVec2{120, 68});
    CHECK(p.target_size(0, {480, 270}) == IVec2{480, 270});
    RenderTargetDesc fixed;
    fixed.size = {64, 32};
    const RenderTargetId b = p.add_target(str_id("b"), fixed);
    CHECK(b == 2);
    CHECK(p.target_size(b, {480, 270}) == IVec2{64, 32});
    CHECK(p.target_size(a, {0, 0}) == IVec2{1, 1});

    p.route_layers(10, 12, a);
    p.route_layers(30, 20, b); // reversed range
    CHECK(p.target_of(9) == 0);
    CHECK(p.target_of(10) == a);
    CHECK(p.target_of(12) == a);
    CHECK(p.target_of(13) == 0);
    CHECK(p.target_of(20) == b);
    CHECK(p.target_of(30) == b);
    p.route_layers(11, 11, 0);
    CHECK(p.target_of(11) == 0);

    // Composites stay sorted by layer; equal layers keep insertion order.
    p.composite(b, kMaterialAlpha, 50);
    p.composite(a, kMaterialMultiply, 40);
    p.composite(a, kMaterialAdditive, 50);
    REQUIRE(p.composites().size() == 3u);
    CHECK(p.composites()[0].layer == 40);
    CHECK(p.composites()[1].source == b);
    CHECK(p.composites()[2].material == kMaterialAdditive);
    for (int i = 3; i <= 7; ++i)
        CHECK(p.add_target(str_id("t" + std::to_string(i)), {}) == i);
    CHECK(p.target_count() == RenderPasses::kMaxTargets);
}

TEST_CASE("lighting: flicker stays within its band") {
    for (int i = 0; i < 200; ++i) {
        const float f = flicker_factor(static_cast<float>(i) * 0.037f, 0.3f, 17);
        CHECK(f >= 0.7f - 1e-4f);
        CHECK(f <= 1.0f + 1e-4f);
    }
    CHECK(flicker_factor(3.0f, 0.0f, 1) == 1.0f);
}

TEST_CASE("lighting: flicker edge cases, determinism and seeds") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(flicker_factor(1.0f, -0.5f, 3) == 1.0f);
    CHECK(flicker_factor(1.0f, nan, 3) == 1.0f);
    const float big = flicker_factor(1.0f, 5.0f, 3); // amount clamps to 1
    CHECK(big >= 0.0f);
    CHECK(big <= 1.0f);
    CHECK(flicker_factor(nan, 0.5f, 3) >= 0.5f);
    CHECK(flicker_factor(2.5f, 0.4f, 9) == flicker_factor(2.5f, 0.4f, 9));
    // Different seeds flicker out of step, and the light moves over time.
    int differ = 0;
    float lo = 1.0f, hi = 0.0f;
    for (int i = 0; i < 100; ++i) {
        const float t = static_cast<float>(i) * 0.05f;
        differ += flicker_factor(t, 0.5f, 1) != flicker_factor(t, 0.5f, 2) ? 1 : 0;
        lo = std::min(lo, flicker_factor(t, 0.5f, 1));
        hi = std::max(hi, flicker_factor(t, 0.5f, 1));
    }
    CHECK(differ > 90);
    CHECK(hi - lo > 0.2f);
}

TEST_CASE("lighting: light_stamp scales alpha by intensity and flicker; radius wobbles a little") {
    Light2D l;
    l.radius = 40;
    l.intensity = 0.5f;
    LightStamp s = light_stamp(l, 1.0f);
    CHECK(s.radius == 40.0f);
    CHECK(s.color.a == 128);
    CHECK(s.color.r == l.color.r);
    l.intensity = 3.0f; // clamped
    CHECK(light_stamp(l, 1.0f).color.a == 255);
    l.intensity = 1.0f;
    l.flicker = 0.5f;
    for (int i = 0; i < 50; ++i) {
        s = light_stamp(l, static_cast<float>(i) * 0.1f);
        CHECK(s.radius >= 40.0f * 0.94f - 1e-3f);
        CHECK(s.radius <= 40.0f + 1e-3f);
        CHECK(s.color.a >= 127);
    }
    l.radius = -5;
    CHECK(light_stamp(l, 0.0f).radius == 0.0f);
    l.radius = std::numeric_limits<float>::infinity();
    CHECK(light_stamp(l, 0.0f).radius == 0.0f);
}

TEST_CASE("lighting: ensure_light_pass creates, routes and composites the light target once") {
    RenderPasses p;
    const RenderTargetId id = ensure_light_pass(p, 0.5f);
    REQUIRE(id != 0);
    CHECK(p.find(kLightTargetName) == id);
    CHECK(p.desc(id).filter == TextureFilter::Bilinear);
    CHECK(p.desc(id).clear_color == kWhite);
    CHECK(p.target_size(id, {480, 270}) == IVec2{240, 135});
    for (int l = kLayerLights; l <= kLayerLights + 8; ++l)
        CHECK(p.target_of(static_cast<std::uint8_t>(l)) == id);
    CHECK(p.target_of(kLayerLightComposite) == 0);
    REQUIRE(p.composites().size() == 1u);
    CHECK(p.composites()[0].material == kMaterialMultiply);
    CHECK(p.composites()[0].layer == kLayerLightComposite);
    const std::uint32_t v = p.version();
    CHECK(ensure_light_pass(p, 0.25f) == id);
    CHECK(p.version() == v);
    CHECK(p.composites().size() == 1u);
    RenderPasses q;
    CHECK(q.desc(ensure_light_pass(q, -1.0f)).virtual_scale == 0.5f);
    CHECK(kLayerLightComposite < kFirstScreenLayer);
}
