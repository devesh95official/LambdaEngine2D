#pragma once
// Fonts and text. Glyphs live in atlas pages, so text batches with sprites.
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <lam/core/asset_id.h>
#include <lam/core/fixed_string.h>
#include <lam/ecs/component.h>
#include <lam/math/color.h>
#include <lam/math/rect.h>
#include <lam/render/sprite.h>
#include <lam/render/texture.h>
namespace lam {
class RenderQueue;
struct Glyph {
    char32_t cp = 0;
    Rect source;                                                // texels in Font::page
    Vec2 size;
    Vec2 offset;                                                // from the pen position to the glyph's top-left
    float advance = 0;
};
// .fnt (BMFont) or .ttf/.otf baked by the backend; "name.ttf#16" picks the size.
struct Font {
    AssetId<Texture> page;
    float line_height = 0;
    float base_size = 0;
    float ascent = 0;
    std::vector<Glyph> glyphs;                                  // sorted by cp
    const Glyph *find(char32_t cp) const;                       // null if missing
};
enum class TextAlign : std::uint8_t { Left, Center, Right };
struct TextStyle {
    AssetId<Font> font;                                         // none -> RenderDefaults::font
    float size = 0;                                             // px; 0 = Font::base_size
    Color color = kWhite;
    std::uint8_t layer = kLayerUi;
    Color shadow = kTransparent;
};
class TextLayout {
public:
    struct Placed { Vec2 pos; std::uint32_t glyph = 0; };       // glyph index into Font::glyphs
    void set(const Font &f, std::string_view utf8, float size, float max_width, TextAlign align);
    bool dirty(std::string_view utf8, float max_width) const;
    // dirty() only checks text and width, so compare these too if the font or size can change.
    const Font *font() const { return font_; }
    float font_size() const { return font_size_; }
    Vec2 size() const;
    int line_count() const;
    int glyph_count() const;
    std::span<const Placed> placed() const;
private:
    std::vector<Placed> placed_;
    Vec2 size_;
    int lines_ = 0;
    std::uint64_t text_hash_ = 0;
    float max_width_ = 0;
    float font_size_ = 0;
    const Font *font_ = nullptr;
};
Vec2 measure_text(const Font &f, std::string_view utf8, float size);   // no allocation; '\n' breaks
void draw_text(RenderQueue &q, const Font &f, std::string_view utf8, Vec2 pos, const TextStyle &s, float depth = 0);
void draw_layout(RenderQueue &q, const Font &f, const TextLayout &l, Vec2 pos, const TextStyle &s,
                 int max_glyphs = -1, float depth = 0);                 // max_glyphs for typewriter text
// Grid of cells in `page`; used by tests and the built-in font.
Font make_monospace_font(AssetId<Texture> page, Vec2 origin, int cell_w, int cell_h, int columns = 16,
                         char32_t first = 32, int count = 96);
struct RenderDefaults { AssetId<Font> font; };                  // built-in 6x8 ASCII font
struct Text {
    FixedString<64> value;
    TextStyle style;
    Vec2 offset;
    TextAlign align = TextAlign::Left;
};
}
LAM_REFLECT_ENUM(lam::TextAlign, "left", "center", "right")
LAM_REFLECT_STRUCT(lam::TextStyle, font, size, color, layer, shadow)
LAM_REFLECT_FLAGS(lam::Text, lam::kSerialize | lam::kSnapshot, value, style, offset, align)
