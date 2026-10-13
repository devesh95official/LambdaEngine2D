// Glyph lookup, measuring, word wrap and glyph quads. Wrapping breaks at spaces, honours '\n'
// and gives long words their own line. Missing glyphs draw as '?'.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <lam/core/assert.h>
#include <lam/core/text.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>

namespace lam {
namespace {

// size <= 0 means the base size.
float scale_of(const Font &f, float size) {
    return (size > 0.0f && f.base_size > 0.0f) ? size / f.base_size : 1.0f;
}

bool is_space(char32_t cp) {
    return cp == U' ' || cp == U'\t';
}

// Falls back to '?', then null.
const Glyph *glyph_for(const Font &f, char32_t cp) {
    const Glyph *g = f.find(cp);
    return g != nullptr ? g : f.find(U'?');
}

float advance_of(const Font &f, char32_t cp, float scale) {
    if (is_space(cp)) {
        const Glyph *space = f.find(U' ');
        return space != nullptr ? space->advance * scale : 0.0f;
    }
    const Glyph *g = glyph_for(f, cp);
    return g != nullptr ? g->advance * scale : 0.0f;
}

// FNV-1a. Never returns 0, which marks a fresh layout.
std::uint64_t text_hash(std::string_view s) {
    std::uint64_t h = 14695981039346656037ULL;
    for (char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

// Calls sink.place() per visible code point and sink.end_line() per line. max_width <= 0
// doesn't wrap.
template <class Sink>
void layout_lines(const Font &f, std::string_view text, float scale, float max_width, Sink &sink) {
    std::size_t at = 0;
    for (;;) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        float pen = 0.0f;           // width up to the end of the last word on this line
        float pending_space = 0.0f; // spaces since that word (or since the paragraph start)
        bool has_word = false;
        std::size_t i = at;
        while (i < end) {
            const Utf8Next n = utf8_next(text, i);
            if (n.cp == U'\r') {
                i += n.bytes;
                continue;
            }
            if (is_space(n.cp)) {
                pending_space += advance_of(f, n.cp, scale);
                i += n.bytes;
                continue;
            }
            std::size_t word_end = i;
            float word_width = 0.0f;
            while (word_end < end) {
                const Utf8Next w = utf8_next(text, word_end);
                if (is_space(w.cp)) break;
                if (w.cp != U'\r') word_width += advance_of(f, w.cp, scale);
                word_end += w.bytes;
            }
            if (has_word && max_width > 0.0f && pen + pending_space + word_width > max_width) {
                sink.end_line(pen, true);
                pen = 0.0f;
                pending_space = 0.0f; // spaces at a wrap point are dropped
            }
            pen += pending_space;
            pending_space = 0.0f;
            while (i < word_end) {
                const Utf8Next w = utf8_next(text, i);
                if (w.cp != U'\r') {
                    sink.place(w.cp, pen);
                    pen += advance_of(f, w.cp, scale);
                }
                i += w.bytes;
            }
            has_word = true;
        }
        sink.end_line(pen, has_word);
        if (end >= text.size()) break;
        at = end + 1;
    }
}

// Trailing empty lines don't count.
struct MeasureSink {
    float widest = 0.0f;
    int lines = 0;
    int kept_lines = 0;
    void place(char32_t, float) {}
    void end_line(float width, bool has_word) {
        ++lines;
        if (has_word) {
            kept_lines = lines;
            widest = std::max(widest, width);
        }
    }
};

void push_glyph(RenderQueue &q, const Font &f, const Glyph &g, Vec2 at, float scale, Color color,
                std::uint8_t layer, float depth) {
    const float w = g.size.x * scale, h = g.size.y * scale;
    const float u0 = g.source.x, v0 = g.source.y, u1 = g.source.x + g.source.w,
                v1 = g.source.y + g.source.h;
    const QuadVertex v[4] = {{{at.x, at.y}, {u0, v0}, color},
                             {{at.x + w, at.y}, {u1, v0}, color},
                             {{at.x + w, at.y + h}, {u1, v1}, color},
                             {{at.x, at.y + h}, {u0, v1}, color}};
    q.push(layer, depth, 0, f.page, v);
}

bool visible(const Glyph &g) {
    return g.size.x > 0.0f && g.size.y > 0.0f;
}

// No wrapping.
void text_pass(RenderQueue &q, const Font &f, std::string_view text, Vec2 origin, float scale,
               Color color, std::uint8_t layer, float depth) {
    const float line_height = f.line_height * scale;
    float x = 0.0f, y = 0.0f;
    for (std::size_t i = 0; i < text.size();) {
        const Utf8Next n = utf8_next(text, i);
        i += n.bytes;
        if (n.cp == U'\n') {
            x = 0.0f;
            y += line_height;
            continue;
        }
        if (n.cp == U'\r') continue;
        if (is_space(n.cp)) {
            x += advance_of(f, n.cp, scale);
            continue;
        }
        const Glyph *g = glyph_for(f, n.cp);
        if (g == nullptr) continue;
        if (visible(*g)) {
            push_glyph(q, f, *g,
                       {origin.x + x + g->offset.x * scale, origin.y + y + g->offset.y * scale},
                       scale, color, layer, depth);
        }
        x += g->advance * scale;
    }
}

// One base pixel, scaled, at least 1.
float shadow_offset(float scale) {
    return std::max(1.0f, std::floor(scale));
}

} // namespace

const Glyph *Font::find(char32_t cp) const {
    const auto it = std::lower_bound(glyphs.begin(), glyphs.end(), cp,
                                     [](const Glyph &g, char32_t c) { return g.cp < c; });
    return (it != glyphs.end() && it->cp == cp) ? &*it : nullptr;
}

Vec2 measure_text(const Font &f, std::string_view utf8, float size) {
    const float scale = scale_of(f, size);
    MeasureSink m;
    layout_lines(f, utf8, scale, 0.0f, m);
    return {m.widest, static_cast<float>(m.kept_lines) * f.line_height * scale};
}

void TextLayout::set(const Font &f, std::string_view utf8, float size, float max_width,
                     TextAlign align) {
    text_hash_ = text_hash(utf8);
    max_width_ = max_width;
    font_size_ = size;
    font_ = &f;
    placed_.clear();

    const float scale = scale_of(f, size);
    const float line_height = f.line_height * scale;
    // Align within the wrap width, or the widest line when not wrapping.
    float box = max_width;
    if (align != TextAlign::Left && !(max_width > 0.0f)) box = measure_text(f, utf8, size).x;

    struct PlaceSink {
        const Font &font;
        std::vector<Placed> &out;
        float scale, line_height, box;
        TextAlign align;
        std::size_t line_start = 0;
        int lines = 0;
        int kept_lines = 0;
        float widest = 0.0f;
        void place(char32_t cp, float x) {
            const Glyph *g = glyph_for(font, cp);
            if (g == nullptr || !visible(*g)) return;
            const float y = static_cast<float>(lines) * line_height;
            out.push_back({{x + g->offset.x * scale, y + g->offset.y * scale},
                           static_cast<std::uint32_t>(g - font.glyphs.data())});
        }
        void end_line(float width, bool has_word) {
            if (align != TextAlign::Left) {
                float shift = box - width;
                if (align == TextAlign::Center) shift *= 0.5f;
                shift = std::max(0.0f, std::floor(shift)); // overlong lines stay left
                for (std::size_t i = line_start; i < out.size(); ++i)
                    out[i].pos.x += shift;
            }
            line_start = out.size();
            ++lines;
            if (has_word) {
                kept_lines = lines;
                widest = std::max(widest, width);
            }
        }
    } sink{f, placed_, scale, line_height, box, align};
    layout_lines(f, utf8, scale, max_width, sink);

    lines_ = sink.kept_lines;
    size_ = {sink.widest, static_cast<float>(lines_) * line_height};
}

bool TextLayout::dirty(std::string_view utf8, float max_width) const {
    return text_hash(utf8) != text_hash_ || max_width != max_width_;
}

Vec2 TextLayout::size() const {
    return size_;
}

int TextLayout::line_count() const {
    return lines_;
}

int TextLayout::glyph_count() const {
    return static_cast<int>(placed_.size());
}

std::span<const TextLayout::Placed> TextLayout::placed() const {
    return placed_;
}

void draw_text(RenderQueue &q, const Font &f, std::string_view utf8, Vec2 pos, const TextStyle &s,
               float depth) {
    const float scale = scale_of(f, s.size);
    if (s.shadow.a > 0) {
        const float o = shadow_offset(scale);
        text_pass(q, f, utf8, {pos.x + o, pos.y + o}, scale, s.shadow, s.layer, depth);
    }
    text_pass(q, f, utf8, pos, scale, s.color, s.layer, depth);
}

void draw_layout(RenderQueue &q, const Font &f, const TextLayout &l, Vec2 pos, const TextStyle &s,
                 int max_glyphs, float depth) {
    // Use the scale the layout was built with so sizes match its positions.
    float scale = scale_of(f, s.size);
    if (l.line_count() > 0 && f.line_height > 0.0f) {
        scale = l.size().y / (static_cast<float>(l.line_count()) * f.line_height);
    }
    const std::span<const TextLayout::Placed> placed = l.placed();
    const std::size_t n = max_glyphs < 0
                              ? placed.size()
                              : std::min(placed.size(), static_cast<std::size_t>(max_glyphs));
    const auto pass = [&](Vec2 origin, Color color) {
        for (std::size_t i = 0; i < n; ++i) {
            const TextLayout::Placed &p = placed[i];
            LAM_ASSERT(p.glyph < f.glyphs.size(),
                       "draw_layout: layout was built with another font");
            if (p.glyph >= f.glyphs.size()) continue;
            push_glyph(q, f, f.glyphs[p.glyph], origin + p.pos, scale, color, s.layer, depth);
        }
    };
    if (s.shadow.a > 0) {
        const float o = shadow_offset(scale);
        pass({pos.x + o, pos.y + o}, s.shadow);
    }
    pass(pos, s.color);
}

Font make_monospace_font(AssetId<Texture> page, Vec2 origin, int cell_w, int cell_h, int columns,
                         char32_t first, int count) {
    LAM_ASSERT(cell_w > 0 && cell_h > 0 && columns > 0,
               "make_monospace_font: cells and columns must be > 0");
    Font f;
    f.page = page;
    f.line_height = static_cast<float>(cell_h);
    f.base_size = static_cast<float>(cell_h);
    f.ascent = static_cast<float>(cell_h);
    if (cell_w <= 0 || cell_h <= 0 || columns <= 0 || count <= 0) return f;
    f.glyphs.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        Glyph g;
        g.cp = first + static_cast<char32_t>(i);
        g.source = {origin.x + static_cast<float>((i % columns) * cell_w),
                    origin.y + static_cast<float>((i / columns) * cell_h),
                    static_cast<float>(cell_w), static_cast<float>(cell_h)};
        g.size = {static_cast<float>(cell_w), static_cast<float>(cell_h)};
        g.advance = static_cast<float>(cell_w);
        f.glyphs.push_back(g);
    }
    return f;
}

} // namespace lam
