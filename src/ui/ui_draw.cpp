// UI drawing: quads in document order (parents under children), clipped for lists and text
// blocks, using flat theme colours or the tinted nine-slice. No allocation.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <lam/core/fixed_string.h>
#include <lam/core/id.h>
#include <lam/core/text.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/ui/dialogue.h>
#include <lam/ui/ui_draw.h>

#include "detail/ui_tree_impl.h"

namespace lam {
namespace {

using Slot = UiTree::Slot;

Rect deflate(const Rect &r, Vec2 pad) {
    return {r.x + pad.x, r.y + pad.y, std::max(0.0f, r.w - pad.x * 2), std::max(0.0f, r.h - pad.y * 2)};
}

IRect to_irect(const Rect &r) {
    const int x0 = static_cast<int>(std::floor(r.x)), y0 = static_cast<int>(std::floor(r.y));
    const int x1 = static_cast<int>(std::ceil(r.x + r.w)), y1 = static_cast<int>(std::ceil(r.y + r.h));
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

bool has_nine_slice(const UiTheme &t) {
    return static_cast<bool>(t.nine_slice.texture) && t.nine_slice.source.w > 0.0f && t.nine_slice.source.h > 0.0f;
}

// A box in the theme's style: nine-slice tinted by `fill`, or a flat fill with a 1 px frame.
void box(RenderQueue &q, const UiTheme &theme, Rect r, Color fill, Color border, std::uint8_t layer) {
    if (r.empty()) return;
    if (has_nine_slice(theme)) {
        draw_nine_slice(q, theme.nine_slice, theme.nine_slice_border, r, fill, layer);
        return;
    }
    if (fill.a > 0) q.rect(r, fill, layer);
    if (border.a > 0 && r.w >= 2.0f && r.h >= 2.0f) q.rect_lines(r, 1.0f, border, layer);
}

struct TextOut {
    RenderQueue &q;
    const Font *font;
    std::uint8_t layer;
    // Text at `pos` (top-left) in `size` pixels; nothing without a font.
    void text(std::string_view s, Vec2 pos, float size, Color c) const {
        if (font == nullptr || s.empty() || c.a == 0) return;
        TextStyle st;
        st.size = size;
        st.color = c;
        st.layer = layer;
        draw_text(q, *font, s, {std::floor(pos.x), std::floor(pos.y)}, st);
    }
    float width(std::string_view s, float size) const {
        return font == nullptr ? 0.0f : measure_text(*font, s, size).x;
    }
};

// Small cache of clip ids so siblings in one list share one clip rectangle.
struct ClipCache {
    RenderQueue &q;
    std::array<IRect, 8> rects{};
    std::array<std::uint8_t, 8> ids{};
    int count = 0, next = 0;
    std::uint8_t current = 0;
    void apply(bool has_clip, const Rect &r) {
        std::uint8_t want = 0;
        if (has_clip) {
            const IRect ir = to_irect(r);
            for (int k = 0; k < count; ++k) {
                if (rects[static_cast<std::size_t>(k)] == ir) {
                    want = ids[static_cast<std::size_t>(k)];
                    break;
                }
            }
            if (want == 0) {
                want = q.clip(ir);
                const std::size_t slot = static_cast<std::size_t>(next);
                rects[slot] = ir;
                ids[slot] = want;
                next = (next + 1) % 8;
                count = std::min(count + 1, 8);
            }
        }
        if (want != current) {
            q.set_clip(want);
            current = want;
        }
    }
};

void draw_glyph(RenderQueue &q, const Font &f, const Glyph &g, Vec2 at, float scale, Color color,
                std::uint8_t layer) {
    const float w = g.size.x * scale, h = g.size.y * scale;
    const float u0 = g.source.x, v0 = g.source.y, u1 = g.source.x + g.source.w, v1 = g.source.y + g.source.h;
    const QuadVertex v[4] = {{{at.x, at.y}, {u0, v0}, color},
                             {{at.x + w, at.y}, {u1, v0}, color},
                             {{at.x + w, at.y + h}, {u1, v1}, color},
                             {{at.x, at.y + h}, {u0, v1}, color}};
    q.push(layer, 0.0f, 0, f.page, v);
}

// Glyphs of a laid-out block, culled to `visible`, at most `max_glyphs` (-1 = all).
void draw_block(RenderQueue &q, const Font &f, const TextLayout &layout, Vec2 origin, float scale, Color color,
                Rect visible, int max_glyphs, std::uint8_t layer) {
    const auto placed = layout.placed();
    const std::size_t n = max_glyphs < 0 ? placed.size()
                                         : std::min(placed.size(), static_cast<std::size_t>(max_glyphs));
    const float lh = f.line_height * scale;
    for (std::size_t i = 0; i < n; ++i) {
        const TextLayout::Placed &p = placed[i];
        if (p.glyph >= f.glyphs.size()) continue;
        const Vec2 at{std::floor(origin.x + p.pos.x), std::floor(origin.y + p.pos.y)};
        if (at.y + lh < visible.y || at.y > visible.y + visible.h) continue;   // off the visible lines
        draw_glyph(q, f, f.glyphs[p.glyph], at, scale, color, layer);
    }
}

void draw_value_widget(RenderQueue &q, const TextOut &out, const Slot &s, const UiThemeClass &c, Color text,
                       float size) {
    const UiNode &n = s.node;
    const Rect v = s.value;
    if (v.empty()) return;
    switch (n.kind) {
    case UiKind::Toggle: {
        q.rect_lines(v, 1.0f, text, out.layer);
        if (n.value >= 0.5f && v.w > 4.0f) q.rect({v.x + 2, v.y + 2, v.w - 4, v.h - 4}, text, out.layer);
        break;
    }
    case UiKind::Slider: {
        const float range = n.max - n.min;
        const float f = range > 0.0f ? std::clamp((n.value - n.min) / range, 0.0f, 1.0f) : 0.0f;
        const float track_h = std::max(2.0f, std::floor(v.h / 4.0f));
        const Rect track{v.x, std::floor(v.y + (v.h - track_h) * 0.5f), v.w, track_h};
        q.rect(track, c.panel_border, out.layer);
        q.rect({track.x, track.y, std::floor(track.w * f), track.h}, text, out.layer);
        const float knob_w = 3.0f;
        q.rect({std::floor(v.x + (v.w - knob_w) * f), v.y, knob_w, v.h}, text, out.layer);
        break;
    }
    case UiKind::Choice: {
        FixedString<32> scratch;
        const std::string_view value = detail::ui_value_text(n, scratch);
        out.text("<", {v.x, v.y}, size, text);
        out.text(">", {v.x + v.w - out.width(">", size), v.y}, size, text);
        const float w = out.width(value, size);
        out.text(value, {v.x + (v.w - w) * 0.5f, v.y}, size, text);
        break;
    }
    default:
        break;
    }
}

} // namespace

void draw_nine_slice(RenderQueue &q, const TextureRegion &region, float border, Rect dest, Color tint,
                     std::uint8_t layer) {
    if (dest.empty() || tint.a == 0) return;
    const Rect src = region.source;
    // Borders never exceed half the source or the destination.
    const float b = std::max(0.0f, std::min({border, src.w * 0.5f, src.h * 0.5f, dest.w * 0.5f, dest.h * 0.5f}));
    const float sx[4] = {src.x, src.x + b, src.x + src.w - b, src.x + src.w};
    const float sy[4] = {src.y, src.y + b, src.y + src.h - b, src.y + src.h};
    const float dx[4] = {dest.x, dest.x + b, dest.x + dest.w - b, dest.x + dest.w};
    const float dy[4] = {dest.y, dest.y + b, dest.y + dest.h - b, dest.y + dest.h};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (dx[col + 1] <= dx[col] || dy[row + 1] <= dy[row]) continue;   // zero-width border
            const QuadVertex v[4] = {{{dx[col], dy[row]}, {sx[col], sy[row]}, tint},
                                     {{dx[col + 1], dy[row]}, {sx[col + 1], sy[row]}, tint},
                                     {{dx[col + 1], dy[row + 1]}, {sx[col + 1], sy[row + 1]}, tint},
                                     {{dx[col], dy[row + 1]}, {sx[col], sy[row + 1]}, tint}};
            q.push(layer, 0.0f, 0, region.texture, v);
        }
    }
}

void draw_ui_tree(RenderQueue &q, const UiTree &tree, const UiTheme &theme, const Font *font,
                  std::uint8_t layer) {
    const auto &slots = UiTreeAccess::slots(tree);
    const auto &order = UiTreeAccess::order(tree);
    const UiId focused = tree.focused();
    const UiId pressed = tree.pressed();
    const UiId hovered = tree.hovered();
    const TextOut out{q, font, layer};
    ClipCache clips{q};
    for (const std::uint16_t i : order) {
        const Slot &s = slots[i];
        if (!s.alive || !s.shown) continue;
        const UiNode &n = s.node;
        const UiId id = detail::ui_id_of(s, i);
        UiThemeClass scratch;
        const UiThemeClass &c = detail::ui_class_of(theme, n, scratch);
        const float size = s.scale * (font != nullptr ? font->base_size : 8.0f);
        const Rect r = s.screen;
        const Rect content = deflate(r, n.style.padding);
        const Color text = n.color.a > 0 && n.kind == UiKind::Label ? n.color
                           : n.enabled                             ? c.text
                                                                   : c.text_disabled;
        const float lh = s.line_height;
        clips.apply(s.has_clip, s.clip);
        switch (n.kind) {
        case UiKind::Panel:
            if (n.color.a > 0) q.rect(r, n.color, layer);
            else if (c.background) box(q, theme, r, c.panel, c.panel_border, layer);
            break;
        case UiKind::List: {
            if (n.color.a > 0) q.rect(r, n.color, layer);
            else if (n.style.theme_class && c.background) box(q, theme, r, c.panel, c.panel_border, layer);
            if (s.max_scroll > 0 && r.h > 4.0f) {               // scroll bar
                const int cols = n.layout == UiLayout::Grid ? std::max<int>(1, n.style.grid_columns) : 1;
                const float page = static_cast<float>(s.page_rows);
                const float total = page + static_cast<float>(s.max_scroll / cols);
                const float track_h = r.h - 2.0f;
                const float thumb_h = std::max(4.0f, std::floor(track_h * page / total));
                const float t = static_cast<float>(n.scroll / cols) / std::max(1.0f, total - page);
                const Rect track{r.x + r.w - 3.0f, r.y + 1.0f, 2.0f, track_h};
                q.rect(track, c.panel_border, layer);
                q.rect({track.x, std::floor(track.y + (track_h - thumb_h) * std::clamp(t, 0.0f, 1.0f)), 2.0f, thumb_h},
                       c.text, layer);
            }
            break;
        }
        case UiKind::Label: {
            const float w = out.width(n.text.view(), size);
            const float th = std::max(lh, s.text_size.y);
            float x = content.x;
            if (c.align == TextAlign::Center) x = content.x + (content.w - w) * 0.5f;
            else if (c.align == TextAlign::Right) x = content.x + content.w - w;
            out.text(n.text.view(), {x, content.y + (content.h - th) * 0.5f}, size, text);
            break;
        }
        case UiKind::Button:
        case UiKind::Toggle:
        case UiKind::Slider:
        case UiKind::Choice:
        case UiKind::TextInput: {
            const bool is_focused = id == focused;
            const bool down = id == pressed && id == hovered;
            Color fill = down ? c.button_down : is_focused ? c.button_focus : c.button;
            if (!n.enabled) fill = scale_alpha(fill, 0.5f);
            if (n.color.a > 0) fill = n.color;
            box(q, theme, r, fill, is_focused ? c.text : c.panel_border, layer);
            const float y = content.y + (content.h - lh) * 0.5f;
            if (n.kind == UiKind::Button) {
                const float w = out.width(n.text.view(), size);
                const TextAlign align = c.align_set ? c.align : TextAlign::Center;
                float x = content.x;
                if (align == TextAlign::Center) x = content.x + (content.w - w) * 0.5f;
                else if (align == TextAlign::Right) x = content.x + content.w - w;
                out.text(n.text.view(), {x, y}, size, text);
            } else if (n.kind == UiKind::TextInput) {
                q.rect(content, c.panel, layer);
                out.text(n.text.view(), {content.x + 1.0f, y}, size, text);
                if (is_focused) {
                    const float w = out.width(n.text.view(), size);
                    out.text("_", {content.x + 1.0f + w, y}, size, text);
                }
            } else {
                out.text(n.text.view(), {content.x, y}, size, text);
                draw_value_widget(q, out, s, c, text, size);
            }
            break;
        }
        case UiKind::Image: {
            const TextureRegion &reg = n.image;
            const Vec2 natural = (reg.size.x > 0 && reg.size.y > 0) ? reg.size : Vec2{reg.source.w, reg.source.h};
            if (natural.x > 0 && natural.y > 0) {
                Color tint = n.color.a > 0 ? n.color : kWhite;
                if (!n.enabled) tint = scale_alpha(tint, 0.5f);
                q.region(reg, {std::floor(r.x), std::floor(r.y)}, {r.w / natural.x, r.h / natural.y}, 0.0f, {0, 0},
                         tint, layer, 0.0f);
            }
            if (n.focusable && id == focused) q.rect_lines(r, 1.0f, c.button_focus, layer);
            break;
        }
        case UiKind::TextBlock: {
            if (n.style.theme_class && c.background) box(q, theme, r, c.panel, c.panel_border, layer);
            if (font == nullptr || !s.block) break;
            const Color tc = n.color.a > 0 ? n.color : text;
            const Vec2 origin{content.x, content.y - static_cast<float>(n.scroll) * lh};
            draw_block(q, *font, s.block->layout, origin, s.scale, tc, s.clip, s.block->reveal, layer);
            break;
        }
        }
    }
    clips.apply(false, {});
}

void draw_toast_list(RenderQueue &q, const ToastQueue &toasts, const UiTheme &theme, const Font *font, Rect screen,
                     std::uint8_t layer) {
    const auto list = toasts.toasts();
    if (list.empty() || font == nullptr) return;
    const float size = theme.font_size;
    const float scale = detail::ui_text_scale(*font, size);
    const float lh = font->line_height * scale;
    const Vec2 pad{6, 3};
    float y = screen.y + screen.h - 6.0f;
    for (std::size_t k = list.size(); k-- > 0;) {
        const ToastQueue::Toast &t = list[k];
        FixedString<80> line;
        if (t.count > 1) format_to(line, "{} x{}", t.text.view(), t.count);
        else line = t.text.view();
        const float w = measure_text(*font, line.view(), size).x + pad.x * 2;
        const float h = lh + pad.y * 2;
        const float slide = t.age < 0.15f ? (1.0f - t.age / 0.15f) * (1.0f - t.age / 0.15f) * 8.0f : 0.0f;
        y -= h;
        const Rect box_r{std::floor(screen.x + (screen.w - w) * 0.5f), std::floor(y + slide), std::floor(w), h};
        const float a = std::clamp(t.alpha, 0.0f, 1.0f);
        y -= 2.0f;
        if (a <= 0.0f) continue;                                // keeps its place, draws nothing
        box(q, theme, box_r, scale_alpha(theme.panel, a), scale_alpha(theme.panel_border, a), layer);
        TextStyle st;
        st.size = size;
        st.color = scale_alpha(t.color, a);
        st.layer = layer;
        draw_text(q, *font, line.view(), {box_r.x + pad.x, box_r.y + pad.y}, st);
    }
}

void draw_message_log(RenderQueue &q, const MessageLog &log, const Font &font, Rect area, float size,
                      std::uint8_t layer) {
    const float lh = font.line_height * detail::ui_text_scale(font, size);
    if (lh <= 0.0f || log.size() == 0) return;
    // Compared in float before converting: an infinite or huge area.h means "every line"
    // NaN and negative heights fit none.
    const float lines = std::floor(area.h / lh);
    const std::size_t fit = std::isnan(lines)                                   ? 0
                            : !(lines < static_cast<float>(log.size())) ? log.size()
                                                                        : static_cast<std::size_t>(std::max(0.0f, lines));
    const std::size_t first = log.size() > fit ? log.size() - fit : 0;
    TextStyle st;
    st.size = size;
    st.layer = layer;
    for (std::size_t i = first; i < log.size(); ++i) {
        st.color = log.color(i);
        draw_text(q, font, log.line(i), {std::floor(area.x), std::floor(area.y + static_cast<float>(i - first) * lh)}, st);
    }
}

void draw_dialogue_box(RenderQueue &q, const DialogueRunner &runner, TextLayout &layout, const UiTheme &theme,
                       const Font &font, Rect area, int selected, std::uint8_t layer) {
    const DialogueLine *line = runner.line();
    if (line == nullptr) return;
    const float size = theme.font_size;
    const float scale = detail::ui_text_scale(font, size);
    const float lh = font.line_height * scale;
    box(q, theme, area, theme.panel, theme.panel_border, layer);
    const Rect content = deflate(area, {6, 4});
    TextStyle st;
    st.size = size;
    st.layer = layer;
    float y = content.y;
    if (line->speaker) {
        st.color = {255, 215, 94, 255};
        draw_text(q, font, id_name(line->speaker), {content.x, y}, st);
        y += lh + 2.0f;
    }
    // Glyph indices and pen positions depend on the font and size too (a theme that finishes
    // loading changes them): re-wrap when either differs.
    if (layout.dirty(line->text, content.w) || layout.font() != &font || layout.font_size() != size)
        layout.set(font, line->text, size, content.w, TextAlign::Left);
    // Glyphs revealed so far: the visible code points that place a glyph (spaces do not).
    const std::string_view shown = runner.visible_text();
    int glyphs = 0;
    for (std::size_t at = 0; at < shown.size();) {
        const Utf8Next nx = utf8_next(shown, at);
        at += nx.bytes;
        if (nx.cp != U' ' && nx.cp != U'\t' && nx.cp != U'\n' && nx.cp != U'\r') ++glyphs;
    }
    draw_block(q, font, layout, {content.x, y}, scale, theme.text, content, glyphs, layer);
    y += layout.size().y + 4.0f;
    if (runner.typing_done() && line->choice_count > 0) {
        for (int k = 0; k < line->choice_count; ++k) {
            const bool sel = k == selected;
            st.color = sel ? theme.text : theme.text_disabled;
            FixedString<64> label;
            format_to(label, "{} {}", sel ? ">" : " ", line->choices[static_cast<std::size_t>(k)].text.view());
            draw_text(q, font, label.view(), {content.x, y}, st);
            y += lh;
        }
    }
}

} // namespace lam
