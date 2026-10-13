// lam_render_gpu_tests: the raylib backend in a hidden 640x360 window, one App per case.
// The golden image is golden/gpu_scene.txt; run with LAM_UPDATE_GOLDEN=1 to rewrite it.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/alloc.h>
#include <lam/core/app.h>
#include <lam/core/platform.h>
#include <lam/core/raylib_platform.h>
#include <lam/render/atlas.h>
#include <lam/render/font.h>
#include <lam/render/lighting.h>
#include <lam/render/render_plan.h>
#include <lam/render/render_plugin.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>
#include <lam/render/shapes.h>
#include <lam/render/texture.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>

#include "bench_scene.h"

namespace {

using namespace lam;

constexpr Color kClear{32, 32, 48, 255};

std::unique_ptr<App> make_app(bool rlgl_batch = false, std::string root = {}) {
    static const char *argv[] = {"lam_render_gpu_tests", "--hidden", "--no-vsync"};
    AppConfig cfg;
    cfg.title = "lam_render_gpu_tests";
    cfg.window_size = {640, 360};
    cfg.virtual_size = {480, 270};
    cfg.audio = false;
    cfg.settings_file = "";
    auto app = std::make_unique<App>(cfg, 3, argv);
    app->add_plugin<PlatformPlugin>();
    AssetsConfig assets;
    assets.root = std::move(root);
    app->add_plugin<AssetsPlugin>(assets);
    RenderConfig rc;
    rc.clear_color = kClear;
    app->add_plugin<RenderPlugin>(rc);
    RaylibRenderConfig rl;
    rl.rlgl_batch = rlgl_batch;
    app->add_plugin<RaylibRenderPlugin>(rl);
    return app;
}

bool has_gpu(App &app) {
    return app.platform().has_gpu();
}

// Allocations between BeginDrawing (FrameBegin -10000) and the swap (FrameEnd +10000) belong to
// the window system and GL driver.
struct PlatformBracket {
    std::uint64_t mark = 0;
    std::uint64_t allocations = 0;
};
void bracket_mark(Ctx &ctx) {
    ctx.res<PlatformBracket>().mark = allocation_count();
}
void bracket_count(Ctx &ctx) {
    PlatformBracket &b = ctx.res<PlatformBracket>();
    b.allocations += allocation_count() - b.mark;
}
void add_once(App &app, Stage stage, SystemFn fn, std::string_view name, int order) {
    SystemDesc d;
    d.stage = stage;
    d.fn = fn;
    d.name = name;
    d.order = order;
    d.scope = SystemScope::Once;
    app.add_system(d);
}
void add_platform_bracket(App &app) {
    app.insert_resource<PlatformBracket>();
    add_once(app, Stage::FrameBegin, bracket_mark, "test.begin_frame.mark", -10001);
    add_once(app, Stage::FrameBegin, bracket_count, "test.begin_frame.count", -9999);
    add_once(app, Stage::FrameEnd, bracket_mark, "test.end_frame.mark", 9999);
    add_once(app, Stage::FrameEnd, bracket_count, "test.end_frame.count", 10001);
}

// Runs `frames` frames and returns the engine's own operator-new calls (driver calls excluded).
std::uint64_t engine_allocations(App &app, int frames, std::uint64_t *driver = nullptr) {
    const std::uint64_t backend0 = app.res<RenderBackendStats>().driver_allocations;
    const std::uint64_t bracket0 = app.res<PlatformBracket>().allocations;
    const AllocationScope scope;
    app.run_frames(frames);
    const std::uint64_t total = scope.count();
    const std::uint64_t outside = (app.res<RenderBackendStats>().driver_allocations - backend0) +
                                  (app.res<PlatformBracket>().allocations - bracket0);
    if (driver != nullptr) *driver = outside;
    return total - outside;
}

// Reads the virtual screen at the end of the next frame.
const ImageData &capture(App &app) {
    ScreenCapture &c = app.res<ScreenCapture>();
    c.request = true;
    c.ready = false;
    app.run_frames(1);
    REQUIRE(c.ready);
    return c.image;
}

Color pixel(const ImageData &img, int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) +
                           static_cast<std::size_t>(x)) *
                          4;
    return {img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]};
}

bool near(Color a, Color b, int tol = 2) {
    const auto d = [&](std::uint8_t x, std::uint8_t y) { return std::abs(int(x) - int(y)) <= tol; };
    return d(a.r, b.r) && d(a.g, b.g) && d(a.b, b.b);
}

// Run-length rows "RRGGBB*n ..." (alpha ignored: the virtual screen is opaque).
std::string rle(const ImageData &img) {
    std::string out;
    char buf[32];
    for (int y = 0; y < img.height; ++y) {
        int x = 0;
        while (x < img.width) {
            const Color c = pixel(img, x, y);
            int n = 1;
            while (x + n < img.width) {
                const Color d = pixel(img, x + n, y);
                if (d.r != c.r || d.g != c.g || d.b != c.b) break;
                ++n;
            }
            std::snprintf(buf, sizeof buf, "%02x%02x%02x*%d ", c.r, c.g, c.b, n);
            out += buf;
            x += n;
        }
        out += '\n';
    }
    return out;
}

// The golden scene's resource and systems (positions in virtual pixels; no camera).
struct Golden {
    AssetId<Texture> checker;
    MaterialId invert = 0;
};

void draw_golden(Ctx &ctx) {
    RenderQueue &q = ctx.res<RenderQueue>();
    const Golden &g = ctx.res<Golden>();
    q.rect({10, 10, 40, 30}, Color{255, 0, 0, 255}, kLayerWorld);
    q.rect({320, 160, 40, 40}, Color{0, 0, 255, 255}, kLayerWorld);
    q.circle({200, 100}, 12, Color{0, 255, 0, 255}, kLayerWorld);
    TextureRegion checker;
    checker.texture = g.checker;
    checker.source = {0, 0, 8, 8};
    checker.size = {8, 8};
    q.region(checker, {100, 60}, {2, 2}, 0, {0.5f, 1.0f}, kWhite, kLayerWorld, 60);
    q.line({10, 120}, {110, 120}, 2, Color{255, 255, 0, 255}, kLayerWorld);
    if (g.invert != 0) {
        TextureRegion white;
        white.texture = q.shape_texture();
        white.source = kWhiteSource;
        white.size = {2, 2};
        q.region(white, {400, 20}, {20, 15}, 0, {0, 0}, Color{255, 0, 0, 255}, kLayerWorld, 0,
                 g.invert);
    }
    q.rect({300, 150, 100, 50}, Color{255, 255, 255, 128}, kLayerUi);
    const Font *font = ctx.res<AssetServer>().get<Font>(ctx.res<RenderDefaults>().font);
    if (font != nullptr) {
        TextStyle s;
        draw_text(q, *font, "GOLDEN 42", {10, 200}, s);
    }
}

AssetId<Texture> add_checker(AssetServer &assets) {
    Texture t;
    t.width = 8;
    t.height = 8;
    t.pixels.width = 8;
    t.pixels.height = 8;
    t.pixels.rgba.resize(8 * 8 * 4);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const bool a = ((x + y) % 2) == 0;
            std::uint8_t *p = &t.pixels.rgba[static_cast<std::size_t>(y * 8 + x) * 4];
            p[0] = a ? 255 : 0;
            p[1] = a ? 128 : 0;
            p[2] = a ? 0 : 255;
            p[3] = 255;
        }
    return assets.add<Texture>(std::move(t), "test/checker");
}

void setup_golden(App &app, MaterialId invert = 0) {
    Golden &g = app.insert_resource<Golden>();
    g.checker = add_checker(app.res<AssetServer>());
    g.invert = invert;
    app.add_system(Stage::Render, draw_golden, "test.golden");
}

// Known colours of the golden scene (both draw paths).
void check_golden_pixels(const ImageData &img) {
    REQUIRE(img.width == 480);
    REQUIRE(img.height == 270);
    CHECK(near(pixel(img, 0, 0), kClear));
    CHECK(near(pixel(img, 479, 269), kClear));
    CHECK(near(pixel(img, 30, 25), Color{255, 0, 0, 255}));   // red rect
    CHECK(near(pixel(img, 9, 25), kClear));                   // just left of it
    CHECK(near(pixel(img, 50, 25), kClear));                  // just right of it (x 10..49)
    CHECK(near(pixel(img, 200, 100), Color{0, 255, 0, 255})); // circle centre
    // Checker sprite: 8x8 texels at scale 2, feet at (100, 60): texel (0, 0) covers
    // (92..93, 44..45).
    CHECK(near(pixel(img, 92, 44), Color{255, 128, 0, 255}));
    CHECK(near(pixel(img, 94, 44), Color{0, 0, 255, 255}));
    CHECK(near(pixel(img, 94, 46), Color{255, 128, 0, 255}));
    CHECK(near(pixel(img, 107, 59), Color{255, 128, 0, 255}));
    CHECK(near(pixel(img, 108, 59), kClear));
    CHECK(near(pixel(img, 60, 120), Color{255, 255, 0, 255})); // line
    // 50% white over blue (UI over world), and over the clear colour.
    CHECK(near(pixel(img, 340, 180), Color{128, 128, 255, 255}, 3));
    CHECK(near(pixel(img, 305, 155), Color{144, 144, 152, 255}, 3));
    // 'G' of the built-in font: column 0 = 0x3E (rows 1..5 lit).
    CHECK(near(pixel(img, 10, 200), kClear));
    CHECK(near(pixel(img, 10, 201), kWhite));
    CHECK(near(pixel(img, 10, 205), kWhite));
    CHECK(near(pixel(img, 10, 206), kClear));
}

} // namespace

TEST_CASE("gpu: the golden scene, the custom material and the light pass") {
    test::TempDir dir;
    test::write_test_file(dir, "shaders/invert.shader.json", R"({"fs": "invert.fs"})");
    test::write_test_file(dir, "shaders/invert.fs",
                          "VARYING vec2 fragTexCoord;\n"
                          "VARYING vec4 fragColor;\n"
                          "uniform sampler2D texture0;\n"
                          "void main() {\n"
                          "    vec4 c = TEXTURE2D(texture0, fragTexCoord) * fragColor;\n"
                          "    FRAG_COLOR = vec4(vec3(1.0) - c.rgb, c.a);\n"
                          "}\n");
    std::unique_ptr<App> app = make_app(false, dir.path());
    REQUIRE(has_gpu(*app));
    Result<AssetId<Shader>> shader =
        app->res<AssetServer>().load_now<Shader>("shaders/invert.shader.json");
    REQUIRE(shader);
    REQUIRE(app->res<AssetServer>().get<Shader>(shader.value())->gpu_id != 0u);
    const MaterialId invert = app->res<Materials>().add(Material{shader.value(), BlendMode::Alpha});
    setup_golden(*app, invert);
    app->run_frames(3);
    const ImageData &img = capture(*app);
    check_golden_pixels(img);
    CHECK(near(pixel(img, 410, 30), Color{0, 255, 255, 255})); // red through the invert shader
    CHECK(test::golden_equal(rle(img), LAM_RENDER_TEST_DIR "/golden/gpu_scene.txt"));
    const RenderStats &st = app->res<RenderStats>();
    CHECK(st.draw_calls >= 3u);
    CHECK(st.draw_calls <= 10u);
    CHECK(st.skipped_unloaded == 0u);

    // The light pass: ambient multiplies everything; a light brightens its centre.
    Lighting &lighting = app->main_world().resources().get<Lighting>();
    lighting.enabled = true;
    lighting.ambient = Color{64, 64, 64, 255};
    const Entity lamp = app->main_world().create();
    app->main_world().add<Transform>(lamp, Transform{{30, 25}});
    Light2D l;
    l.color = kWhite;
    l.radius = 40;
    app->main_world().add<Light2D>(lamp, l);
    app->run_frames(2);
    const ImageData &lit = capture(*app);
    const Color centre = pixel(lit, 30, 25); // red rect under the light
    const Color dark = pixel(lit, 400, 250); // clear colour, ambient only
    CHECK(centre.r > 200);
    CHECK(near(dark, Color{8, 8, 12, 255}, 3));
    CHECK(pixel(lit, 200, 100).g < 80); // the circle is far from the lamp: ambient only
}

TEST_CASE("gpu: the bench scene draws in 10 or fewer draw calls and allocates nothing per frame") {
    for (const int sprites : {10000, 50000}) {
        CAPTURE(sprites);
        std::unique_ptr<App> app = make_app();
        REQUIRE(has_gpu(*app));
        bench::setup(*app, sprites);
        app->run_frames(60);
        const RenderStats &st = app->res<RenderStats>();
        CHECK(st.quads > static_cast<std::uint32_t>(sprites));
        CHECK(st.skipped_unloaded == 0u);
        // Planned batches plus the present; tiles, sprites, glyphs and shapes share one page.
        CHECK(st.draw_calls == st.batches + 1u);
        CHECK(st.draw_calls <= 10u);
        // The whole picture is drawn: tiles, sprites, additive particles, the UI panel.
        const ImageData &img = capture(*app);
        CHECK_FALSE(near(pixel(img, 20, 230), kClear)); // UI panel
        add_platform_bracket(*app);
        app->run_frames(2);
        if (allocation_counting_enabled()) {
            std::uint64_t driver = 0;
            const std::uint64_t own = engine_allocations(*app, 600, &driver);
            MESSAGE("bench scene ", sprites, " sprites: ", st.draw_calls, " draw calls, ",
                    st.batches, " batches; 600 frames: ", own, " engine allocations, ", driver,
                    " inside GL/window-system calls");
            CHECK(own == 0u);
        } else {
            MESSAGE("allocation counting is off (no lam_alloc_hook)");
        }
    }
}

TEST_CASE("gpu: a .ttf is baked at load, packed into the atlas and drawn") {
#ifndef LAM_RENDER_TEST_TTF
    MESSAGE("no TrueType font available; skipped");
#else
    std::FILE *in = std::fopen(LAM_RENDER_TEST_TTF, "rb");
    if (in == nullptr) {
        MESSAGE("missing " LAM_RENDER_TEST_TTF "; skipped");
        return;
    }
    std::string bytes;
    char buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, in)) > 0;)
        bytes.append(buf, n);
    std::fclose(in);
    test::TempDir dir;
    test::write_test_file(dir, "fonts/mono.ttf", bytes);
    std::unique_ptr<App> app = make_app(false, dir.path());
    REQUIRE(has_gpu(*app));
    AssetServer &assets = app->res<AssetServer>();
    Result<AssetId<Font>> id = assets.load_now<Font>("fonts/mono.ttf#20");
    REQUIRE(id);
    const Font *font = assets.get<Font>(id.value());
    REQUIRE(font != nullptr);
    CHECK(font->base_size == 20.0f);
    CHECK(font->glyphs.size() == 95u);
    CHECK(font->page == app->res<AtlasSet>().shape_page()); // packed with everything else
    const Glyph *w = font->find(U'W');
    REQUIRE(w != nullptr);
    CHECK(w->size.x > 4.0f);
    CHECK(w->advance > 4.0f);
    CHECK(measure_text(*font, "WW", 0).x == doctest::Approx(2.0f * w->advance));
    struct Label {
        AssetId<Font> font;
    };
    app->insert_resource<Label>(Label{id.value()});
    app->add_system(
        Stage::RenderUI,
        [](Ctx &ctx) {
            const Font *f = ctx.res<AssetServer>().get<Font>(ctx.res<Label>().font);
            if (f != nullptr) draw_text(ctx.res<RenderQueue>(), *f, "WWWW", {20, 20}, TextStyle{});
        },
        "test.ttf_label");
    app->run_frames(3);
    const ImageData &img = capture(*app);
    int lit = 0;
    for (int y = 20; y < 44; ++y)
        for (int x = 20; x < 100; ++x)
            lit += pixel(img, x, y).r > 200 ? 1 : 0;
    CHECK(lit > 80);
    CHECK(near(pixel(img, 300, 200), kClear));
#endif
}

TEST_CASE("gpu: the rlgl-batch fallback draws the same golden scene") {
    std::unique_ptr<App> app = make_app(true);
    REQUIRE(has_gpu(*app));
    setup_golden(*app);
    app->run_frames(3);
    const ImageData &img = capture(*app);
    check_golden_pixels(img);
    CHECK(app->res<RenderStats>().draw_calls <= 10u);
}

namespace {
void draw_triangles(Ctx &ctx) {
    RenderQueue &q = ctx.res<RenderQueue>();
    // Clockwise on screen (screen layer, virtual px) and counter-clockwise in the world (default
    // view: world px = virtual px).
    draw_triangle(q, {100, 100}, {160, 100}, {100, 160}, Color{255, 0, 0, 255}, kLayerUi);
    draw_triangle(q, {300, 100}, {300, 160}, {360, 100}, Color{0, 255, 0, 255}, kLayerWorld);
}
} // namespace

TEST_CASE("gpu: draw_triangle fills half of its box in either winding, on both draw paths") {
    for (const bool rlgl : {false, true}) {
        CAPTURE(rlgl);
        std::unique_ptr<App> app = make_app(rlgl);
        REQUIRE(has_gpu(*app));
        app->add_system(Stage::Render, draw_triangles, "test.triangles");
        app->run_frames(2);
        const ImageData &img = capture(*app);
        CHECK(near(pixel(img, 110, 110), Color{255, 0, 0, 255}));
        CHECK(near(pixel(img, 120, 130), Color{255, 0, 0, 255}));
        CHECK(near(pixel(img, 150, 150), kClear)); // the other half of the box
        CHECK(near(pixel(img, 310, 110), Color{0, 255, 0, 255}));
        CHECK(near(pixel(img, 350, 150), kClear));
        CHECK(near(pixel(img, 200, 200), kClear));
    }
}

// ---- Regressions --------------------------------------------------------------------------------

namespace {

void put_be32(std::string &s, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        s += static_cast<char>((v >> shift) & 0xFFu);
}
void put_le32(std::string &s, std::uint32_t v) {
    for (int shift = 0; shift <= 24; shift += 8)
        s += static_cast<char>((v >> shift) & 0xFFu);
}

std::uint32_t crc32_of(std::string_view bytes) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (const char ch : bytes) {
        c ^= static_cast<std::uint8_t>(ch);
        for (int k = 0; k < 8; ++k)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// A w x h PNG of one colour (RGBA8, stored deflate blocks): real bytes for raylib's decoder.
std::string solid_png(int w, int h, Color col) {
    std::string raw;
    for (int y = 0; y < h; ++y) {
        raw += '\0'; // filter: none
        for (int x = 0; x < w; ++x) {
            raw += static_cast<char>(col.r);
            raw += static_cast<char>(col.g);
            raw += static_cast<char>(col.b);
            raw += static_cast<char>(col.a);
        }
    }
    REQUIRE(raw.size() <= 65535u); // one stored block
    std::string z = "\x78\x01";
    z += '\x01'; // final block, stored
    const auto len = static_cast<std::uint32_t>(raw.size());
    const std::uint32_t nlen = ~len & 0xFFFFu;
    z += static_cast<char>(len & 0xFFu);
    z += static_cast<char>((len >> 8) & 0xFFu);
    z += static_cast<char>(nlen & 0xFFu);
    z += static_cast<char>((nlen >> 8) & 0xFFu);
    z += raw;
    std::uint32_t a = 1, b = 0;
    for (const char ch : raw) {
        a = (a + static_cast<std::uint8_t>(ch)) % 65521u;
        b = (b + a) % 65521u;
    }
    put_be32(z, (b << 16) | a);
    const auto chunk = [](std::string &out, std::string_view type, std::string_view data) {
        put_be32(out, static_cast<std::uint32_t>(data.size()));
        std::string td(type);
        td += data;
        out += td;
        put_be32(out, crc32_of(td));
    };
    std::string ihdr;
    put_be32(ihdr, static_cast<std::uint32_t>(w));
    put_be32(ihdr, static_cast<std::uint32_t>(h));
    ihdr += std::string("\x08\x06\x00\x00\x00", 5); // 8 bit, RGBA, deflate, no filter, no interlace
    std::string png("\x89PNG\r\n\x1a\n", 8);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

// A DXT1-compressed .dds (w, h multiples of 4): w*h/2 bytes of block data, no mipmaps.
std::string dxt1_dds(int w, int h) {
    std::uint32_t hdr[31] = {};
    const std::uint32_t linear = static_cast<std::uint32_t>((w / 4) * (h / 4) * 8);
    hdr[0] = 124;                                     // size
    hdr[1] = 0x1u | 0x2u | 0x4u | 0x1000u | 0x80000u; // caps|height|width|pixelformat|linearsize
    hdr[2] = static_cast<std::uint32_t>(h);
    hdr[3] = static_cast<std::uint32_t>(w);
    hdr[4] = linear;
    hdr[18] = 32;          // ddspf.size
    hdr[19] = 0x4u;        // ddspf.flags: FOURCC
    hdr[20] = 0x31545844u; // "DXT1"
    hdr[26] = 0x1000u;     // caps: texture
    std::string dds = "DDS ";
    for (const std::uint32_t v : hdr)
        put_le32(dds, v);
    dds.append(linear, '\x55');
    return dds;
}

Texture solid_texture(IVec2 size, Color col) {
    Texture t;
    t.width = size.x;
    t.height = size.y;
    t.pixels.width = size.x;
    t.pixels.height = size.y;
    for (int i = 0; i < size.x * size.y; ++i) {
        t.pixels.rgba.push_back(col.r);
        t.pixels.rgba.push_back(col.g);
        t.pixels.rgba.push_back(col.b);
        t.pixels.rgba.push_back(col.a);
    }
    return t;
}

constexpr Color kRed{255, 0, 0, 255};
constexpr Color kGreen{0, 255, 0, 255};

// One texture drawn 4x at (200, 100), top-left pivot: texel (x, y) covers (200 + 4x, 100 + 4y).
struct ShownTexture {
    AssetId<Texture> id;
    IVec2 size{4, 4};
};
void draw_shown_texture(Ctx &ctx) {
    const ShownTexture &s = ctx.res<ShownTexture>();
    TextureRegion r;
    r.texture = s.id;
    r.source = {0, 0, static_cast<float>(s.size.x), static_cast<float>(s.size.y)};
    r.size = {static_cast<float>(s.size.x), static_cast<float>(s.size.y)};
    ctx.res<RenderQueue>().region(r, {200, 100}, {4, 4}, 0, {0, 0}, kWhite, kLayerWorld, 0);
}

} // namespace

TEST_CASE("gpu: a reloaded .png is uploaded again (same id, Texture::version 1)") {
    test::TempDir dir;
    test::write_test_file(dir, "tex/hero.png", solid_png(4, 4, kRed));
    std::unique_ptr<App> app = make_app(false, dir.path());
    REQUIRE(has_gpu(*app));
    AssetServer &assets = app->res<AssetServer>();
    Result<AssetId<Texture>> id = assets.load_now<Texture>("tex/hero.png");
    REQUIRE(id);
    app->insert_resource<ShownTexture>(ShownTexture{id.value(), {4, 4}});
    app->add_system(Stage::Render, draw_shown_texture, "test.shown_texture");
    app->run_frames(2);
    CHECK(near(pixel(capture(*app), 201, 101), kRed));
    test::write_test_file(dir, "tex/hero.png", solid_png(4, 4, kGreen));
    REQUIRE(assets.reload(id.value()).ok());
    CHECK(assets.get<Texture>(id.value())->version == 1u); // a fresh decode: version not carried
    const ImageData &img = capture(*app);
    CHECK(near(pixel(img, 201, 101), kGreen));
    CHECK(near(pixel(img, 214, 114), kGreen));
}

TEST_CASE("gpu: a compressed (DXT1 .dds) image is refused, not copied as RGBA8") {
    std::unique_ptr<App> app = make_app(); // installs the raylib image decoder
    const std::string dds = dxt1_dds(16, 16);
    const Result<ImageData> r = decode_image(
        std::span<const std::byte>(reinterpret_cast<const std::byte *>(dds.data()), dds.size()),
        ".dds");
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Unsupported);
    // An uncompressed image still decodes.
    const std::string png = solid_png(2, 2, kGreen);
    const Result<ImageData> ok = decode_image(
        std::span<const std::byte>(reinterpret_cast<const std::byte *>(png.data()), png.size()),
        ".png");
    REQUIRE(ok);
    CHECK(ok->width == 2);
    CHECK(ok->rgba.size() == 16u);
    CHECK(ok->rgba[1] == 255);
}

namespace {
// Frees a texture's slot and refills it with a new texture in the same Update (scene change).
struct SlotSwap {
    AssetScope scope = kRootScope;
    bool now = false;
    bool reused = false;
};
void swap_slot(Ctx &ctx) {
    SlotSwap &s = ctx.res<SlotSwap>();
    if (!s.now) return;
    s.now = false;
    AssetServer &assets = ctx.res<AssetServer>();
    ShownTexture &shown = ctx.res<ShownTexture>();
    const AssetId<Texture> old = shown.id;
    assets.end_scope(s.scope);
    assets.collect_garbage(); // Removed {i, g}; index i is free again
    shown.id = assets.add<Texture>(solid_texture({4, 4}, kGreen), "", kRootScope);
    s.reused = shown.id.index == old.index && shown.id.generation != old.generation;
}
} // namespace

TEST_CASE("gpu: a slot freed and reused in one frame never draws the old texture") {
    std::unique_ptr<App> app = make_app();
    REQUIRE(has_gpu(*app));
    AssetServer &assets = app->res<AssetServer>();
    SlotSwap &swap = app->insert_resource<SlotSwap>();
    swap.scope = assets.begin_scope();
    const AssetId<Texture> a = assets.add<Texture>(solid_texture({4, 4}, kRed), "", swap.scope);
    app->insert_resource<ShownTexture>(ShownTexture{a, {4, 4}});
    app->add_system(Stage::Update, swap_slot, "test.swap_slot");
    app->add_system(Stage::Render, draw_shown_texture, "test.shown_texture");
    app->run_frames(2);
    CHECK(near(pixel(capture(*app), 201, 101), kRed));
    app->res<SlotSwap>().now = true;
    const ImageData &img = capture(*app); // the swap happens inside this frame's Update
    REQUIRE(app->res<SlotSwap>().reused);
    CHECK(near(pixel(img, 201, 101), kGreen));
    CHECK(app->res<RenderStats>().skipped_unloaded == 0u);
}

TEST_CASE("gpu: pixels that do not match the texture size are not uploaded") {
    std::unique_ptr<App> app = make_app();
    REQUIRE(has_gpu(*app));
    AssetServer &assets = app->res<AssetServer>();
    const AssetId<Texture> id = assets.add<Texture>(solid_texture({8, 8}, kRed));
    app->insert_resource<ShownTexture>(ShownTexture{id, {8, 8}});
    app->add_system(Stage::Render, draw_shown_texture, "test.shown_texture");
    app->run_frames(2);
    CHECK(near(pixel(capture(*app), 201, 101), kRed));
    // Regenerated at 4x4, but width/height left at 8x8: a 64-byte buffer for a 256-byte upload.
    Texture *t = assets.get_mut<Texture>(id);
    REQUIRE(t != nullptr);
    t->pixels = solid_texture({4, 4}, kGreen).pixels;
    ++t->version;
    assets.mark_modified(id);
    const ImageData &img = capture(*app);
    CHECK(near(pixel(img, 201, 101), kRed)); // the old texture is kept
    CHECK(near(pixel(img, 230, 130), kRed));
    // A matching buffer is uploaded.
    t = assets.get_mut<Texture>(id);
    REQUIRE(t != nullptr);
    t->pixels = solid_texture({8, 8}, kGreen).pixels;
    ++t->version;
    assets.mark_modified(id);
    CHECK(near(pixel(capture(*app), 201, 101), kGreen));
}

namespace {
// A material whose shader asset is reloaded twice inside one Update with new sources; GL hands
// the second compile the program name of the first, so a cache keyed by name alone goes stale.
struct ShaderSwap {
    test::TempDir *dir = nullptr;
    AssetId<Shader> shader;
    MaterialId material = 0;
    bool now = false;
    bool reloaded = false;
    std::uint32_t old_gpu_id = 0, new_gpu_id = 0;
};
void swap_shader(Ctx &ctx) {
    ShaderSwap &s = ctx.res<ShaderSwap>();
    if (!s.now) return;
    s.now = false;
    AssetServer &assets = ctx.res<AssetServer>();
    s.old_gpu_id = assets.get<Shader>(s.shader)->gpu_id;
    test::write_test_file(*s.dir, "shaders/m.shader.json", R"({"fs": "green.fs"})");
    s.reloaded = assets.reload(s.shader).ok();
    s.reloaded = assets.reload(s.shader).ok() && s.reloaded;
    s.new_gpu_id = assets.get<Shader>(s.shader)->gpu_id;
}
void draw_with_material(Ctx &ctx) {
    RenderQueue &q = ctx.res<RenderQueue>();
    TextureRegion white;
    white.texture = q.shape_texture();
    white.source = kWhiteSource;
    white.size = {2, 2};
    q.region(white, {400, 200}, {20, 15}, 0, {0, 0}, kRed, kLayerWorld, 0,
             ctx.res<ShaderSwap>().material);
}
} // namespace

TEST_CASE("gpu: a shader recompiled under a reused program name gets fresh "
          "uniform locations") {
    test::TempDir dir;
    // Vertex stage with eight extra uniforms declared (and used) first: its engine uniforms sit at
    // other locations than in the engine vertex stage.
    std::string padded;
    for (int i = 0; i < 8; ++i)
        padded += "uniform vec4 a_pad" + std::to_string(i) + ";\n";
    padded +=
        "ATTRIBUTE vec2 vertexPosition;\n"
        "ATTRIBUTE vec2 vertexTexCoord;\n"
        "ATTRIBUTE vec4 vertexColor;\n"
        "uniform mat4 mvp;\n"
        "uniform vec4 u_view_x;\n"
        "uniform vec4 u_view_y;\n"
        "uniform float u_snap;\n"
        "uniform vec2 u_inv_texture_size;\n"
        "VARYING vec2 fragTexCoord;\n"
        "VARYING vec4 fragColor;\n"
        "void main() {\n"
        "    vec2 p = vec2(dot(u_view_x.xy, vertexPosition) + u_view_x.z,\n"
        "                  dot(u_view_y.xy, vertexPosition) + u_view_y.z);\n"
        "    p = mix(p, floor(p + 0.5), u_snap);\n"
        "    fragTexCoord = vertexTexCoord * u_inv_texture_size;\n"
        "    fragColor = vertexColor + a_pad0 + a_pad1 + a_pad2 + a_pad3 + a_pad4 + a_pad5 +\n"
        "                a_pad6 + a_pad7;\n"
        "    gl_Position = mvp * vec4(p, 0.0, 1.0);\n"
        "}\n";
    test::write_test_file(dir, "shaders/padded.vs", padded);
    test::write_test_file(dir, "shaders/green.fs",
                          "VARYING vec2 fragTexCoord;\n"
                          "VARYING vec4 fragColor;\n"
                          "uniform sampler2D texture0;\n"
                          "void main() {\n"
                          "    vec4 c = TEXTURE2D(texture0, fragTexCoord) * fragColor;\n"
                          "    FRAG_COLOR = vec4(c.g, c.r, c.b, c.a);\n"
                          "}\n");
    test::write_test_file(dir, "shaders/m.shader.json", R"({"vs": "padded.vs"})");
    std::unique_ptr<App> app = make_app(false, dir.path());
    REQUIRE(has_gpu(*app));
    AssetServer &assets = app->res<AssetServer>();
    Result<AssetId<Shader>> shader = assets.load_now<Shader>("shaders/m.shader.json");
    REQUIRE(shader);
    ShaderSwap &swap = app->insert_resource<ShaderSwap>();
    swap.dir = &dir;
    swap.shader = shader.value();
    swap.material = app->res<Materials>().add(Material{shader.value(), BlendMode::Alpha});
    app->add_system(Stage::Update, swap_shader, "test.swap_shader");
    app->add_system(Stage::Render, draw_with_material, "test.material_quad");
    app->run_frames(2);
    CHECK(near(pixel(capture(*app), 410, 210), kRed)); // padded vertex stage, engine fragment
    app->res<ShaderSwap>().now = true;
    const ImageData &img = capture(*app); // both reloads happen inside this frame's Update
    const ShaderSwap &done = app->res<ShaderSwap>();
    REQUIRE(done.reloaded);
    if (done.new_gpu_id != done.old_gpu_id)
        MESSAGE("the GL driver did not reuse the program name; the stale case is not exercised");
    CHECK(near(pixel(img, 410, 210), kGreen)); // engine vertex stage, red/green swapped
    CHECK(near(pixel(img, 380, 190), kClear));
}
