// Built-in 6x8 font, the BMFont loader and the headless .ttf/.otf placeholder. Glyphs go into the
// "default" atlas so text batches with sprites.
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/data.h>
#include <lam/core/log.h>
#include <lam/render/atlas.h>
#include <lam/render/font.h>

#include "detail/render_internal.h"

namespace lam::detail {
namespace {

// Public-domain 5x7 terminal glyphs for 32..127, five column bytes each, bit 0 = top row.
constexpr std::array<std::uint8_t, 96 * 5> kGlyphColumns = {
    0x00, 0x00, 0x00, 0x00, 0x00, /* ' ' */ 0x00, 0x00, 0x5F, 0x00, 0x00, /* '!' */
    0x00, 0x07, 0x00, 0x07, 0x00, /* '"' */ 0x14, 0x7F, 0x14, 0x7F, 0x14, /* '#' */
    0x24, 0x2A, 0x7F, 0x2A, 0x12, /* '$' */ 0x23, 0x13, 0x08, 0x64, 0x62, /* '%' */
    0x36, 0x49, 0x55, 0x22, 0x50, /* '&' */ 0x00, 0x05, 0x03, 0x00, 0x00, /* ''' */
    0x00, 0x1C, 0x22, 0x41, 0x00, /* '(' */ 0x00, 0x41, 0x22, 0x1C, 0x00, /* ')' */
    0x08, 0x2A, 0x1C, 0x2A, 0x08, /* '*' */ 0x08, 0x08, 0x3E, 0x08, 0x08, /* '+' */
    0x00, 0x50, 0x30, 0x00, 0x00, /* ',' */ 0x08, 0x08, 0x08, 0x08, 0x08, /* '-' */
    0x00, 0x60, 0x60, 0x00, 0x00, /* '.' */ 0x20, 0x10, 0x08, 0x04, 0x02, /* '/' */
    0x3E, 0x51, 0x49, 0x45, 0x3E, /* '0' */ 0x00, 0x42, 0x7F, 0x40, 0x00, /* '1' */
    0x42, 0x61, 0x51, 0x49, 0x46, /* '2' */ 0x21, 0x41, 0x45, 0x4B, 0x31, /* '3' */
    0x18, 0x14, 0x12, 0x7F, 0x10, /* '4' */ 0x27, 0x45, 0x45, 0x45, 0x39, /* '5' */
    0x3C, 0x4A, 0x49, 0x49, 0x30, /* '6' */ 0x01, 0x71, 0x09, 0x05, 0x03, /* '7' */
    0x36, 0x49, 0x49, 0x49, 0x36, /* '8' */ 0x06, 0x49, 0x49, 0x29, 0x1E, /* '9' */
    0x00, 0x36, 0x36, 0x00, 0x00, /* ':' */ 0x00, 0x56, 0x36, 0x00, 0x00, /* ';' */
    0x08, 0x14, 0x22, 0x41, 0x00, /* '<' */ 0x14, 0x14, 0x14, 0x14, 0x14, /* '=' */
    0x00, 0x41, 0x22, 0x14, 0x08, /* '>' */ 0x02, 0x01, 0x51, 0x09, 0x06, /* '?' */
    0x32, 0x49, 0x79, 0x41, 0x3E, /* '@' */ 0x7E, 0x11, 0x11, 0x11, 0x7E, /* 'A' */
    0x7F, 0x49, 0x49, 0x49, 0x36, /* 'B' */ 0x3E, 0x41, 0x41, 0x41, 0x22, /* 'C' */
    0x7F, 0x41, 0x41, 0x22, 0x1C, /* 'D' */ 0x7F, 0x49, 0x49, 0x49, 0x41, /* 'E' */
    0x7F, 0x09, 0x09, 0x09, 0x01, /* 'F' */ 0x3E, 0x41, 0x49, 0x49, 0x7A, /* 'G' */
    0x7F, 0x08, 0x08, 0x08, 0x7F, /* 'H' */ 0x00, 0x41, 0x7F, 0x41, 0x00, /* 'I' */
    0x20, 0x40, 0x41, 0x3F, 0x01, /* 'J' */ 0x7F, 0x08, 0x14, 0x22, 0x41, /* 'K' */
    0x7F, 0x40, 0x40, 0x40, 0x40, /* 'L' */ 0x7F, 0x02, 0x0C, 0x02, 0x7F, /* 'M' */
    0x7F, 0x04, 0x08, 0x10, 0x7F, /* 'N' */ 0x3E, 0x41, 0x41, 0x41, 0x3E, /* 'O' */
    0x7F, 0x09, 0x09, 0x09, 0x06, /* 'P' */ 0x3E, 0x41, 0x51, 0x21, 0x5E, /* 'Q' */
    0x7F, 0x09, 0x19, 0x29, 0x46, /* 'R' */ 0x46, 0x49, 0x49, 0x49, 0x31, /* 'S' */
    0x01, 0x01, 0x7F, 0x01, 0x01, /* 'T' */ 0x3F, 0x40, 0x40, 0x40, 0x3F, /* 'U' */
    0x1F, 0x20, 0x40, 0x20, 0x1F, /* 'V' */ 0x3F, 0x40, 0x38, 0x40, 0x3F, /* 'W' */
    0x63, 0x14, 0x08, 0x14, 0x63, /* 'X' */ 0x07, 0x08, 0x70, 0x08, 0x07, /* 'Y' */
    0x61, 0x51, 0x49, 0x45, 0x43, /* 'Z' */ 0x00, 0x7F, 0x41, 0x41, 0x00, /* '[' */
    0x02, 0x04, 0x08, 0x10, 0x20, /* '\' */ 0x00, 0x41, 0x41, 0x7F, 0x00, /* ']' */
    0x04, 0x02, 0x01, 0x02, 0x04, /* '^' */ 0x40, 0x40, 0x40, 0x40, 0x40, /* '_' */
    0x00, 0x01, 0x02, 0x04, 0x00, /* '`' */ 0x20, 0x54, 0x54, 0x54, 0x78, /* 'a' */
    0x7F, 0x48, 0x44, 0x44, 0x38, /* 'b' */ 0x38, 0x44, 0x44, 0x44, 0x20, /* 'c' */
    0x38, 0x44, 0x44, 0x48, 0x7F, /* 'd' */ 0x38, 0x54, 0x54, 0x54, 0x18, /* 'e' */
    0x08, 0x7E, 0x09, 0x01, 0x02, /* 'f' */ 0x0C, 0x52, 0x52, 0x52, 0x3E, /* 'g' */
    0x7F, 0x08, 0x04, 0x04, 0x78, /* 'h' */ 0x00, 0x44, 0x7D, 0x40, 0x00, /* 'i' */
    0x20, 0x40, 0x44, 0x3D, 0x00, /* 'j' */ 0x7F, 0x10, 0x28, 0x44, 0x00, /* 'k' */
    0x00, 0x41, 0x7F, 0x40, 0x00, /* 'l' */ 0x7C, 0x04, 0x18, 0x04, 0x78, /* 'm' */
    0x7C, 0x08, 0x04, 0x04, 0x78, /* 'n' */ 0x38, 0x44, 0x44, 0x44, 0x38, /* 'o' */
    0x7C, 0x14, 0x14, 0x14, 0x08, /* 'p' */ 0x08, 0x14, 0x14, 0x18, 0x7C, /* 'q' */
    0x7C, 0x08, 0x04, 0x04, 0x08, /* 'r' */ 0x48, 0x54, 0x54, 0x54, 0x20, /* 's' */
    0x04, 0x3F, 0x44, 0x40, 0x20, /* 't' */ 0x3C, 0x40, 0x40, 0x20, 0x7C, /* 'u' */
    0x1C, 0x20, 0x40, 0x20, 0x1C, /* 'v' */ 0x3C, 0x40, 0x30, 0x40, 0x3C, /* 'w' */
    0x44, 0x28, 0x10, 0x28, 0x44, /* 'x' */ 0x0C, 0x50, 0x50, 0x50, 0x3C, /* 'y' */
    0x44, 0x64, 0x54, 0x4C, 0x44, /* 'z' */ 0x00, 0x08, 0x36, 0x41, 0x00, /* '{' */
    0x00, 0x00, 0x7F, 0x00, 0x00, /* '|' */ 0x00, 0x41, 0x36, 0x08, 0x00, /* '}' */
    0x08, 0x04, 0x08, 0x10, 0x08, /* '~' */ 0x7F, 0x41, 0x41, 0x41, 0x7F, /* DEL: a box */
};

// ---- BMFont text parsing ------------------------------------------------------------------------

// One "key=value" attribute of a BMFont line; quoted values keep their spaces.
struct Attr {
    std::string_view key, value;
};

// Up to 24 attributes; extra ones are dropped.
std::size_t split_line(std::string_view line, std::string_view *tag, std::array<Attr, 24> &out) {
    std::size_t i = 0;
    const auto skip = [&] {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
            ++i;
    };
    skip();
    const std::size_t tag_start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t')
        ++i;
    *tag = line.substr(tag_start, i - tag_start);
    std::size_t n = 0;
    while (i < line.size()) {
        skip();
        if (i >= line.size()) break;
        const std::size_t k = i;
        while (i < line.size() && line[i] != '=' && line[i] != ' ' && line[i] != '\t')
            ++i;
        const std::string_view key = line.substr(k, i - k);
        std::string_view value;
        if (i < line.size() && line[i] == '=') {
            ++i;
            if (i < line.size() && line[i] == '"') {
                const std::size_t v = ++i;
                while (i < line.size() && line[i] != '"')
                    ++i;
                value = line.substr(v, i - v);
                if (i < line.size()) ++i;
            } else {
                const std::size_t v = i;
                while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
                    ++i;
                value = line.substr(v, i - v);
            }
        }
        if (n < out.size() && !key.empty()) out[n++] = Attr{key, value};
    }
    return n;
}

bool find_int(const std::array<Attr, 24> &attrs, std::size_t n, std::string_view key, int *out) {
    for (std::size_t i = 0; i < n; ++i) {
        if (attrs[i].key != key) continue;
        const std::string_view v = attrs[i].value;
        int value = 0;
        const auto r = std::from_chars(v.data(), v.data() + v.size(), value);
        if (r.ec != std::errc() || r.ptr != v.data() + v.size()) return false;
        *out = value;
        return true;
    }
    return false;
}

std::string_view find_str(const std::array<Attr, 24> &attrs, std::size_t n, std::string_view key) {
    for (std::size_t i = 0; i < n; ++i) {
        if (attrs[i].key == key) return attrs[i].value;
    }
    return {};
}

std::string extension_of(std::string_view file) {
    const std::size_t dot = file.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string ext(file.substr(dot));
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

// Decodes the page image on the worker and keeps it for finalize to pack into the atlas.
class BmFontLoader final : public AssetLoader<Font> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExtensions[] = {".fnt"};
        return kExtensions;
    }
    Result<Font> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        std::string page_file;
        Result<Font> font = parse_bmfont(text, ctx.path(), &page_file);
        if (!font) return font;
        Result<std::vector<std::byte>> page_bytes = ctx.read(page_file);
        if (!page_bytes) {
            return make_error(page_bytes.error().code,
                              "page '" + page_file + "': " + page_bytes.error().message);
        }
        Result<ImageData> image = decode_image(page_bytes.value(), extension_of(page_file));
        if (!image) {
            return make_error(image.error().code,
                              "page '" + page_file + "': " + image.error().message);
        }
        pending_.put(ctx.path(), std::move(image.value()));
        return font;
    }
    Status finalize(Font &font, FinalizeContext &ctx) override {
        ImageData image;
        if (!pending_.take(ctx.path, &image)) {
            return make_error(ErrorCode::Invalid,
                              std::string(ctx.path) + ": font page image missing at finalize");
        }
        return place_font_image(font, std::move(image), ctx);
    }

private:
    PendingByPath<ImageData> pending_;
};

// Headless stand-in for .ttf/.otf: monospace blocks at the right size, so layout stays plausible.
class PlaceholderTtfLoader final : public AssetLoader<Font> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExtensions[] = {".ttf", ".otf"};
        return kExtensions;
    }
    Result<Font> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        (void)bytes;
        const float size = static_cast<float>(font_size_from_path(ctx.path()));
        Font f;
        f.line_height = size;
        f.base_size = size;
        f.ascent = std::floor(size * 0.8f);
        f.glyphs.reserve(95);
        for (char32_t cp = 32; cp < 127; ++cp) {
            Glyph g;
            g.cp = cp;
            g.source = kWhiteSource;
            g.size = cp == U' ' ? Vec2{} : Vec2{std::floor(size * 0.5f) - 1.0f, f.ascent};
            g.offset = {0.0f, size - f.ascent};
            g.advance = size * 0.5f;
            f.glyphs.push_back(g);
        }
        return f;
    }
    Status finalize(Font &font, FinalizeContext &ctx) override {
        if (const AtlasSet *atlas = ctx.app.try_res<AtlasSet>()) font.page = atlas->shape_page();
        return {};
    }
};

class PlaceholderShaderLoader final : public AssetLoader<Shader> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExtensions[] = {".shader.json"};
        return kExtensions;
    }
    Result<Shader> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return parse_shader_json(text, ctx.path());
    }
};

} // namespace

// ---- Built-in font ------------------------------------------------------------------------------

ImageData builtin_font_image() {
    ImageData image;
    image.width = kBuiltinFontImageSize.x;
    image.height = kBuiltinFontImageSize.y;
    image.rgba.assign(static_cast<std::size_t>(image.width * image.height * 4), 0);
    for (int g = 0; g < kBuiltinFontCount; ++g) {
        const int cx = (g % kBuiltinFontColumns) * kBuiltinFontCellW;
        const int cy = (g / kBuiltinFontColumns) * kBuiltinFontCellH;
        for (int col = 0; col < 5; ++col) {
            const std::uint8_t bits = kGlyphColumns[static_cast<std::size_t>(g * 5 + col)];
            for (int row = 0; row < 8; ++row) {
                if ((bits & (1u << row)) == 0) continue;
                const std::size_t i =
                    (static_cast<std::size_t>(cy + row) * static_cast<std::size_t>(image.width) +
                     static_cast<std::size_t>(cx + col)) *
                    4;
                image.rgba[i + 0] = 255;
                image.rgba[i + 1] = 255;
                image.rgba[i + 2] = 255;
                image.rgba[i + 3] = 255;
            }
        }
    }
    return image;
}

AssetId<Font> add_builtin_font(AssetServer &assets, AtlasSet &atlas) {
    ImageData sized; // size only; the backend blits the pixels
    sized.width = kBuiltinFontImageSize.x;
    sized.height = kBuiltinFontImageSize.y;
    Result<TextureRegion> region = atlas.add(assets, sized, str_id("default"));
    AssetId<Texture> page;
    Vec2 origin;
    if (region) {
        page = region->texture;
        origin = region->source.min();
    }
    Font f = make_monospace_font(page, origin, kBuiltinFontCellW, kBuiltinFontCellH,
                                 kBuiltinFontColumns, 32, kBuiltinFontCount);
    f.ascent = 7;
    return assets.add<Font>(std::move(f), "@font/builtin", kRootScope);
}

void ensure_page_pixels(Texture &page) {
    if (page.pixels.has_pixels() || page.width <= 0 || page.height <= 0) return;
    page.pixels.width = page.width;
    page.pixels.height = page.height;
    page.pixels.rgba.assign(
        static_cast<std::size_t>(page.width) * static_cast<std::size_t>(page.height) * 4, 0);
    blit_into(page, builtin_image(), {0, 0});
}

void blit_into(Texture &page, const ImageData &image, IVec2 at) {
    if (!image.has_pixels() || !page.pixels.has_pixels()) return;
    const int pw = page.pixels.width, ph = page.pixels.height;
    for (int y = 0; y < image.height; ++y) {
        const int ty = at.y + y;
        if (ty < 0 || ty >= ph) continue;
        const int x0 = std::max(0, -at.x);
        const int x1 = std::min(image.width, pw - at.x);
        if (x1 <= x0) continue;
        std::memcpy(page.pixels.rgba.data() +
                        (static_cast<std::size_t>(ty) * static_cast<std::size_t>(pw) +
                         static_cast<std::size_t>(at.x + x0)) *
                            4,
                    image.rgba.data() +
                        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                         static_cast<std::size_t>(x0)) *
                            4,
                    static_cast<std::size_t>(x1 - x0) * 4);
    }
}

// ---- BMFont --------------------------------------------------------------------------------------

Result<Font> parse_bmfont(std::string_view text, std::string_view file_name,
                          std::string *page_file) {
    const auto fail = [&](std::size_t line_no, std::string_view why) -> Result<Font> {
        return make_error(ErrorCode::Parse, std::string(file_name) + ":" + std::to_string(line_no) +
                                                ": " + std::string(why));
    };
    if (text.substr(0, 3) == "BMF")
        return fail(1, "binary BMFont files are not supported (export as text)");
    Font font;
    int pages = -1;
    int scale_w = 0, scale_h = 0;
    bool have_common = false;
    page_file->clear();
    std::array<Attr, 24> attrs{};
    std::size_t at = 0, line_no = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = text.substr(at, end - at);
        at = end + 1;
        ++line_no;
        std::string_view tag;
        const std::size_t n = split_line(line, &tag, attrs);
        if (tag.empty()) continue;
        if (tag == "info") {
            int size = 0;
            if (find_int(attrs, n, "size", &size))
                font.base_size = static_cast<float>(std::abs(size));
        } else if (tag == "common") {
            int line_height = 0, base = 0;
            if (!find_int(attrs, n, "lineHeight", &line_height) || line_height <= 0)
                return fail(line_no, "common: lineHeight missing or not positive");
            if (!find_int(attrs, n, "base", &base)) base = line_height;
            find_int(attrs, n, "pages", &pages);
            find_int(attrs, n, "scaleW", &scale_w);
            find_int(attrs, n, "scaleH", &scale_h);
            font.line_height = static_cast<float>(line_height);
            font.ascent = static_cast<float>(base);
            have_common = true;
        } else if (tag == "page") {
            int id = -1;
            if (!find_int(attrs, n, "id", &id)) return fail(line_no, "page: id missing");
            if (id != 0) return fail(line_no, "only single-page BMFonts are supported");
            const std::string_view file = find_str(attrs, n, "file");
            if (file.empty()) return fail(line_no, "page: file missing");
            *page_file = std::string(file);
        } else if (tag == "char") {
            int id = 0, x = 0, y = 0, w = 0, h = 0, xo = 0, yo = 0, adv = 0, page = 0;
            if (!find_int(attrs, n, "id", &id) || !find_int(attrs, n, "x", &x) ||
                !find_int(attrs, n, "y", &y) || !find_int(attrs, n, "width", &w) ||
                !find_int(attrs, n, "height", &h) || !find_int(attrs, n, "xoffset", &xo) ||
                !find_int(attrs, n, "yoffset", &yo) || !find_int(attrs, n, "xadvance", &adv)) {
                return fail(line_no, "char: id, x, y, width, height, xoffset, yoffset and "
                                     "xadvance are required integers");
            }
            find_int(attrs, n, "page", &page);
            if (page != 0) return fail(line_no, "char on a page other than 0");
            if (id < 0 || id > 0x10FFFF) return fail(line_no, "char: id is not a code point");
            if (x < 0 || y < 0 || w < 0 || h < 0) return fail(line_no, "char: negative rectangle");
            if ((scale_w > 0 && x + w > scale_w) || (scale_h > 0 && y + h > scale_h))
                return fail(line_no, "char: rectangle outside the page");
            Glyph g;
            g.cp = static_cast<char32_t>(id);
            g.source = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                        static_cast<float>(h)};
            g.size = {static_cast<float>(w), static_cast<float>(h)};
            g.offset = {static_cast<float>(xo), static_cast<float>(yo)};
            g.advance = static_cast<float>(adv);
            font.glyphs.push_back(g);
        }
        // Kerning and unknown tags are ignored.
    }
    if (!have_common) return fail(line_no, "no 'common' line");
    if (pages > 1) return fail(line_no, "only single-page BMFonts are supported");
    if (page_file->empty()) return fail(line_no, "no 'page' line");
    if (font.glyphs.empty()) return fail(line_no, "no glyphs");
    std::stable_sort(font.glyphs.begin(), font.glyphs.end(),
                     [](const Glyph &a, const Glyph &b) { return a.cp < b.cp; });
    for (std::size_t i = 1; i < font.glyphs.size(); ++i) {
        if (font.glyphs[i].cp == font.glyphs[i - 1].cp)
            return fail(line_no, "char " + std::to_string(font.glyphs[i].cp) + " listed twice");
    }
    if (font.base_size <= 0.0f) font.base_size = font.line_height;
    return font;
}

int font_size_from_path(std::string_view path, int fallback) {
    int size = fallback;
    const std::size_t hash = path.rfind('#');
    if (hash != std::string_view::npos) {
        const std::string_view frag = path.substr(hash + 1);
        int v = 0;
        const auto r = std::from_chars(frag.data(), frag.data() + frag.size(), v);
        if (r.ec == std::errc() && r.ptr == frag.data() + frag.size()) size = v;
    }
    return std::clamp(size, 4, 256);
}

Status place_font_image(Font &font, ImageData image, FinalizeContext &ctx) {
    Vec2 origin;
    AtlasSet *atlas = ctx.app.try_res<AtlasSet>();
    Result<TextureRegion> region =
        atlas != nullptr ? atlas->add(ctx.server, image, str_id("default"))
                         : Result<TextureRegion>(make_error(ErrorCode::Invalid, "no atlas"));
    if (region) {
        font.page = region->texture;
        origin = region->source.min();
    } else {
        // Too big for a page: keep it as its own texture.
        Texture t;
        t.width = image.width;
        t.height = image.height;
        t.filter = TextureFilter::Point;
        t.pixels = std::move(image);
        const std::string name = std::string(ctx.path) + "#page";
        font.page = ctx.server.add<Texture>(std::move(t), name);
    }
    for (Glyph &g : font.glyphs) {
        g.source.x += origin.x;
        g.source.y += origin.y;
    }
    return {};
}

Result<Shader> parse_shader_json(std::string_view text, std::string_view file_name) {
    return parse_data<Shader>(text, file_name, [](DataReader &in) {
        Shader s;
        s.vs_path = in.text_or("vs", "");
        s.fs_path = in.text_or("fs", "");
        return s;
    });
}

std::unique_ptr<AssetLoader<Font>> make_bmfont_loader() {
    return std::make_unique<BmFontLoader>();
}

std::unique_ptr<AssetLoader<Font>> make_placeholder_ttf_loader() {
    return std::make_unique<PlaceholderTtfLoader>();
}

std::unique_ptr<AssetLoader<Shader>> make_placeholder_shader_loader() {
    return std::make_unique<PlaceholderShaderLoader>();
}

} // namespace lam::detail
