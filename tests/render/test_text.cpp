// Text layout, glyph quads and the font loaders (.fnt, headless .ttf and the built-in 6x8 font).
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/render/atlas.h>
#include <lam/render/font.h>
#include <lam/render/render_plugin.h>
#include <lam/render/render_queue.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>

#include "detail/render_internal.h"

namespace {

using namespace lam;

struct Fonts {
    test::HeadlessApp h;
    const Font *font = nullptr;
    Fonts() {
        h.add<AssetsPlugin>();
        AssetServer &assets = h.app().res<AssetServer>();
        font = assets.get<Font>(test::add_test_font(assets));
        REQUIRE(font != nullptr);
    }
};

// The text of each line of a layout, rebuilt from its placed glyphs (spaces are not placed, so
// they come back from the gaps between pen positions).
std::vector<std::string> lines_of(const Font &f, const TextLayout &l) {
    std::vector<std::string> out(static_cast<std::size_t>(l.line_count()));
    std::vector<float> pen(out.size(), 0.0f);
    for (const TextLayout::Placed &p : l.placed()) {
        const auto line = static_cast<std::size_t>(p.pos.y / f.line_height);
        REQUIRE(line < out.size());
        while (pen[line] + 0.5f < p.pos.x) {
            out[line] += ' ';
            pen[line] += 6.0f;
        }
        out[line] += static_cast<char>(f.glyphs[p.glyph].cp);
        pen[line] = p.pos.x + 6.0f;
    }
    return out;
}

// PNG signature plus an IHDR chunk. The headless decoder only reads the size.
std::string png_header(int w, int h) {
    std::string s("\x89PNG\r\n\x1a\n", 8);
    const auto be32 = [&](std::uint32_t v) {
        for (int i = 3; i >= 0; --i)
            s += static_cast<char>((v >> (8 * i)) & 0xFFu);
    };
    be32(13);
    s += "IHDR";
    be32(static_cast<std::uint32_t>(w));
    be32(static_cast<std::uint32_t>(h));
    s += std::string("\x08\x06\x00\x00\x00", 5);
    be32(0); // CRC (not checked by the header reader)
    return s;
}

constexpr std::string_view kFnt =
    "info face=\"Tiny\" size=10 bold=0\n"
    "common lineHeight=12 base=9 scaleW=64 scaleH=32 pages=1\n"
    "page id=0 file=\"tiny.png\"\n"
    "chars count=3\n"
    "char id=66 x=8 y=0 width=7 height=9 xoffset=0 yoffset=1 xadvance=8 page=0\n"
    "char id=65 x=0 y=0 width=7 height=9 xoffset=0 yoffset=1 xadvance=8 page=0\n"
    "char id=32 x=0 y=0 width=0 height=0 xoffset=0 yoffset=0 xadvance=4 page=0\n"
    "kernings count=0\n";

} // namespace

TEST_CASE("text: wrap breaks at spaces and honours newlines") {
    Fonts f;
    TextLayout l;
    // 6 px per glyph: a width of 60 px holds 10 characters.
    l.set(*f.font, "the quick brown fox", 0, 60, TextAlign::Left);
    REQUIRE(l.line_count() == 2);
    CHECK(lines_of(*f.font, l) == std::vector<std::string>{"the quick", "brown fox"});
    CHECK(l.size() == Vec2{54, 16});

    l.set(*f.font, "a\n\nb", 0, 60, TextAlign::Left);
    REQUIRE(l.line_count() == 3);
    CHECK(lines_of(*f.font, l) == std::vector<std::string>{"a", "", "b"});

    // A word longer than the width keeps a line of its own and is not split.
    l.set(*f.font, "supercalifragilistic word", 0, 30, TextAlign::Left);
    REQUIRE(l.line_count() == 2);
    CHECK(lines_of(*f.font, l)[0] == "supercalifragilistic");

    l.set(*f.font, "", 0, 30, TextAlign::Left);
    CHECK(l.line_count() == 0);
    CHECK(l.glyph_count() == 0);
    // Trailing empty lines are dropped.
    l.set(*f.font, "x\n\n", 0, 30, TextAlign::Left);
    CHECK(l.line_count() == 1);
}

TEST_CASE(
    "text: layouts re-lay out only when the text or width changes; alignment shifts whole pixels") {
    Fonts f;
    TextLayout l;
    l.set(*f.font, "ab cd", 0, 100, TextAlign::Center);
    CHECK_FALSE(l.dirty("ab cd", 100));
    CHECK(l.dirty("ab ce", 100));
    CHECK(l.dirty("ab cd", 90));
    // "ab cd" is 30 px wide in a 100 px box: centred at (100 - 30) / 2 = 35.
    CHECK(l.placed()[0].pos.x == 35.0f);
    l.set(*f.font, "ab", 0, 101, TextAlign::Right);
    CHECK(l.placed()[0].pos.x == 89.0f);
    // Size 16 on an 8 px font doubles everything.
    l.set(*f.font, "ab", 16, 0, TextAlign::Left);
    CHECK(l.size() == Vec2{24, 16});
}

TEST_CASE("text: measure_text counts lines and the widest one, without allocating") {
    Fonts f;
    CHECK(measure_text(*f.font, "abc", 0) == Vec2{18, 8});
    CHECK(measure_text(*f.font, "abc\nde", 0) == Vec2{18, 16});
    CHECK(measure_text(*f.font, "abc", 16) == Vec2{36, 16});
    CHECK(measure_text(*f.font, "", 0) == Vec2{0, 0});
    Vec2 m;
    CHECK_NO_ALLOC(m = measure_text(*f.font, "a fairly long line of text\nand another", 0));
    CHECK(m.y == 16.0f);
}

TEST_CASE("text: draw_text pushes one quad per visible glyph; a shadow pass comes first") {
    Fonts f;
    RenderQueue q(256);
    TextStyle s;
    draw_text(q, *f.font, "a b\nc", {10, 20}, s);
    REQUIRE(q.size() == 3u);
    CHECK(q.quads()[0].v[0].pos == Vec2{10, 20});
    CHECK(q.quads()[1].v[0].pos == Vec2{22, 20}); // after "a "
    CHECK(q.quads()[2].v[0].pos == Vec2{10, 28}); // next line
    CHECK(key_layer(q.quads()[0].key) == kLayerUi);
    CHECK(key_texture(q.quads()[0].key) == texture_slot(f.font->page));

    q.clear();
    s.shadow = Color{0, 0, 0, 200};
    s.color = Color{255, 255, 0, 255};
    draw_text(q, *f.font, "hi", {0, 0}, s);
    REQUIRE(q.size() == 4u);
    CHECK(q.quads()[0].v[0].color == s.shadow);
    CHECK(q.quads()[0].v[0].pos == Vec2{1, 1});
    CHECK(q.quads()[2].v[0].color == s.color);

    // draw_layout with max_glyphs types the text out.
    q.clear();
    TextLayout l;
    l.set(*f.font, "hello world", 0, 0, TextAlign::Left);
    draw_layout(q, *f.font, l, {0, 0}, TextStyle{}, 4);
    CHECK(q.size() == 4u);
    // A code point the font lacks draws as '?'.
    q.clear();
    draw_text(q, *f.font, "\xE2\x82\xAC", {0, 0}, TextStyle{}); // U+20AC
    REQUIRE(q.size() == 1u);
    const Glyph *question = f.font->find(U'?');
    REQUIRE(question != nullptr);
    CHECK(q.quads()[0].v[0].uv == question->source.min());
}

TEST_CASE("fonts: the built-in font image has the 5x7 glyphs in 6x8 cells") {
    const ImageData img = detail::builtin_font_image();
    REQUIRE(img.width == 96);
    REQUIRE(img.height == 48);
    REQUIRE(img.has_pixels());
    const auto alpha = [&](int x, int y) {
        return img.rgba[(static_cast<std::size_t>(y) * 96 + static_cast<std::size_t>(x)) * 4 + 3];
    };
    // ' ' (cell 0) is empty; '!' (cell 1) has its stem in column 2 of the cell, rows 0..4.
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 6; ++x)
            CHECK(alpha(x, y) == 0);
    CHECK(alpha(6 + 2, 0) == 255);
    CHECK(alpha(6 + 2, 4) == 255);
    CHECK(alpha(6 + 2, 5) == 0);
    // The sixth column and eighth row of every cell are spacing.
    int lit = 0;
    for (int g = 0; g < 96; ++g) {
        const int cx = (g % 16) * 6, cy = (g / 16) * 8;
        for (int y = 0; y < 8; ++y) {
            CHECK(alpha(cx + 5, cy + y) == 0);
            for (int x = 0; x < 5; ++x)
                lit += alpha(cx + x, cy + y) != 0 ? 1 : 0;
        }
        for (int x = 0; x < 6; ++x)
            CHECK(alpha(cx + x, cy + 7) == 0);
    }
    CHECK(lit > 500);
}

TEST_CASE("fonts: font_size_from_path reads the '#size' fragment, clamped") {
    CHECK(detail::font_size_from_path("fonts/a.ttf") == 16);
    CHECK(detail::font_size_from_path("fonts/a.ttf#24") == 24);
    CHECK(detail::font_size_from_path("fonts/a.ttf#2") == 4);
    CHECK(detail::font_size_from_path("fonts/a.ttf#9999") == 256);
    CHECK(detail::font_size_from_path("fonts/a.ttf#12px") == 16);
    CHECK(detail::font_size_from_path("fonts/a.ttf#", 10) == 10);
}

TEST_CASE("fonts: parse_bmfont reads glyphs sorted by code point and rejects broken files") {
    std::string page;
    Result<Font> f = detail::parse_bmfont(kFnt, "tiny.fnt", &page);
    REQUIRE(f);
    CHECK(page == "tiny.png");
    CHECK(f->line_height == 12.0f);
    CHECK(f->ascent == 9.0f);
    CHECK(f->base_size == 10.0f);
    REQUIRE(f->glyphs.size() == 3u);
    CHECK(f->glyphs[0].cp == U' ');
    CHECK(f->glyphs[1].cp == U'A');
    CHECK(f->glyphs[2].cp == U'B');
    CHECK(f->glyphs[2].source == Rect{8, 0, 7, 9});
    CHECK(f->glyphs[2].offset == Vec2{0, 1});
    CHECK(f->glyphs[2].advance == 8.0f);

    const auto fails = [&](std::string_view text, std::string_view needle) {
        std::string p;
        Result<Font> r = detail::parse_bmfont(text, "bad.fnt", &p);
        if (r) return false;
        CAPTURE(r.error().message);
        return r.error().message.find("bad.fnt") != std::string::npos &&
               r.error().message.find(std::string(needle)) != std::string::npos;
    };
    CHECK(fails("BMF\x03", "binary"));
    CHECK(fails(
        "info size=10\npage id=0 file=\"a.png\"\nchar id=65 x=0 y=0 width=1 height=1 xoffset=0 "
        "yoffset=0 xadvance=1\n",
        "common"));
    CHECK(fails(
        "common lineHeight=10 base=8 pages=2\npage id=0 file=\"a.png\"\npage id=1 file=\"b.png\"\n",
        "single-page"));
    CHECK(fails("common lineHeight=10 base=8 scaleW=16 scaleH=16\npage id=0 file=\"a.png\"\n"
                "char id=65 x=10 y=0 width=8 height=8 xoffset=0 yoffset=0 xadvance=8\n",
                "outside the page"));
    CHECK(fails("common lineHeight=10 base=8\npage id=0 file=\"a.png\"\nchar id=65 x=0 y=0\n",
                "required"));
    CHECK(fails("common lineHeight=10 base=8\npage id=0 file=\"a.png\"\n"
                "char id=65 x=0 y=0 width=1 height=1 xoffset=0 yoffset=0 xadvance=1\n"
                "char id=65 x=0 y=0 width=1 height=1 xoffset=0 yoffset=0 xadvance=1\n",
                "twice"));
    CHECK(fails("common lineHeight=10 base=8\npage id=0 file=\"a.png\"\n", "no glyphs"));
}

TEST_CASE("fonts: a .fnt loads headless, its page packed into the default atlas group") {
    test::TempDir dir;
    test::write_test_file(dir, "fonts/tiny.fnt", kFnt);
    test::write_test_file(dir, "fonts/tiny.png", png_header(64, 32));
    AssetsConfig cfg;
    cfg.root = dir.path();
    test::HeadlessApp h;
    h.add<AssetsPlugin>(cfg);
    h.add<RenderPlugin>();
    AssetServer &assets = h.app().res<AssetServer>();
    Result<AssetId<Font>> id = assets.load_now<Font>("fonts/tiny.fnt");
    REQUIRE(id);
    const Font *font = assets.get<Font>(id.value());
    REQUIRE(font != nullptr);
    const AtlasSet &atlas = h.app().res<AtlasSet>();
    CHECK(font->page == atlas.shape_page()); // the first default page, shared with shapes
    // Glyph sources moved by the page image's placement in the atlas.
    const Glyph *b = font->find(U'B');
    REQUIRE(b != nullptr);
    CHECK(b->source.w == 7.0f);
    CHECK(b->source.x >= 8.0f);
    CHECK((b->source.x > 8.0f || b->source.y > 0.0f));

    test::write_test_file(dir, "fonts/broken.fnt",
                          "common lineHeight=10\npage id=0 file=\"missing.png\"\n"
                          "char id=65 x=0 y=0 width=1 height=1 xoffset=0 yoffset=0 xadvance=1\n");
    Result<AssetId<Font>> broken = assets.load_now<Font>("fonts/broken.fnt");
    REQUIRE_FALSE(broken);
    CHECK(broken.error().message.find("missing.png") != std::string::npos);
}

TEST_CASE("fonts: headless .ttf is a monospace placeholder with the requested pixel size") {
    test::TempDir dir;
    test::write_test_file(dir, "fonts/body.ttf", "not really a font");
    AssetsConfig cfg;
    cfg.root = dir.path();
    test::HeadlessApp h;
    h.add<AssetsPlugin>(cfg);
    h.add<RenderPlugin>();
    AssetServer &assets = h.app().res<AssetServer>();
    Result<AssetId<Font>> id = assets.load_now<Font>("fonts/body.ttf#12");
    REQUIRE(id);
    const Font *font = assets.get<Font>(id.value());
    REQUIRE(font != nullptr);
    CHECK(font->line_height == 12.0f);
    CHECK(font->base_size == 12.0f);
    CHECK(font->page == h.app().res<AtlasSet>().shape_page());
    CHECK(measure_text(*font, "abcd", 0) == Vec2{24, 12});
    const Glyph *a = font->find(U'a');
    REQUIRE(a != nullptr);
    CHECK(a->source == kWhiteSource);
}

TEST_CASE("atlas: an image wider or taller than a page is refused without int overflow") {
    // A corrupt PNG header can claim up to 0x7FFFFFFF texels; reserved + padding + size must not
    // wrap around into "fits".
    AtlasPacker packer(2048, 1, {64, 64});
    CHECK_FALSE(packer.add({0x7FFFFFFF, 16}).has_value());
    CHECK_FALSE(packer.add({16, 0x7FFFFFFF}).has_value());
    CHECK_FALSE(packer.add({0x7FFFFFFF, 0x7FFFFFFF}).has_value());
    CHECK_FALSE(packer.add({2049, 1}).has_value());
    CHECK(packer.page_count() == 0);
    // The packer is untouched: the next image goes beside the reserved block of page 0.
    const std::optional<AtlasPacker::Placement> p = packer.add({16, 16});
    REQUIRE(p);
    CHECK(p->page == 0);
    CHECK(p->rect == IRect{65, 0, 16, 16});
    CHECK(packer.add({2048, 1983}).has_value()); // still fits below the block: 64 + 1 + 1983 = 2048
}
