// UiTree layout: measure (natural sizes, bottom-up, text cached per node), then arrange (screen
// rects, top-down). Also hit testing and list scrolling. No allocation in steady state.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <lam/core/text.h>
#include <lam/render/font.h>
#include <lam/ui/ui.h>

#include "detail/ui_tree_impl.h"

namespace lam {
namespace detail {

const Font &ui_fallback_font() {
    // Metrics only (6x8 cells over slot 0); never drawn.
    static const Font font = make_monospace_font(AssetId<Texture>{}, {0, 0}, 6, 8, 16, 32, 96);
    return font;
}

float ui_text_scale(const Font &f, float size) {
    return (size > 0.0f && f.base_size > 0.0f) ? size / f.base_size : 1.0f;
}

const UiThemeClass &ui_class_of(const UiTheme &theme, const UiNode &n, UiThemeClass &scratch) {
    if (const UiThemeClass *c = theme.find_class(n.style.theme_class)) return *c;
    scratch = theme.base_class();
    return scratch;
}

std::string_view ui_value_text(const UiNode &n, FixedString<32> &scratch) {
    switch (n.kind) {
    case UiKind::Toggle:
        return n.value >= 0.5f ? "On" : "Off";
    case UiKind::Choice:
        if (n.choice_count > 0) {
            const int count = std::min<int>(n.choice_count, kMaxUiChoices);
            const int i = std::clamp(static_cast<int>(n.value), 0, count - 1);
            return n.choices[static_cast<std::size_t>(i)].view();
        }
        format_to(scratch, "{}", static_cast<long long>(std::lround(n.value)));
        return scratch.view();
    case UiKind::Slider:
        format_to(scratch, "{}", static_cast<long long>(std::lround(n.value)));
        return scratch.view();
    default:
        return {};
    }
}

} // namespace detail

namespace {

using Slot = UiTree::Slot;
using Slots = std::vector<UiTree::Slot>;

struct LayoutCtx {
    Slots &slots;
    const UiTheme &theme;
    const Font &font;
    std::uint32_t &measures;
    std::uint32_t &wraps;
};

Rect deflate(const Rect &r, Vec2 pad) {
    return {r.x + pad.x, r.y + pad.y, std::max(0.0f, r.w - pad.x * 2), std::max(0.0f, r.h - pad.y * 2)};
}

float snap(float v) {
    return std::floor(v);
}

void hide_subtree(Slots &slots, std::uint16_t i) {
    Slot &s = slots[i];
    s.shown = false;
    for (std::uint16_t c = s.first_child; c != kUiNil; c = slots[c].next) hide_subtree(slots, c);
}

// Font size (pixels; 0 = base) and alignment of a node's theme class.
void class_metrics(const LayoutCtx &lc, Slot &s) {
    UiThemeClass scratch;
    const UiThemeClass &c = detail::ui_class_of(lc.theme, s.node, scratch);
    const float size = c.font_size > 0.0f ? c.font_size : lc.theme.font_size;
    s.scale = detail::ui_text_scale(lc.font, size);
    s.line_height = lc.font.line_height * s.scale;
}

Vec2 label_size(const LayoutCtx &lc, Slot &s) {
    if (s.measured_font != &lc.font || s.measured_scale != s.scale || !(s.measured_text == s.node.text)) {
        s.text_size = measure_text(lc.font, s.node.text.view(), s.scale * lc.font.base_size);
        s.measured_text = s.node.text;
        s.measured_font = &lc.font;
        s.measured_scale = s.scale;
        ++lc.measures;
    }
    return s.text_size;
}

float choice_area(const LayoutCtx &lc, const Slot &s) {
    float widest = 0.0f;
    if (s.node.choice_count > 0) {
        const std::size_t count = std::min<std::size_t>(s.node.choice_count, kMaxUiChoices);
        for (std::size_t i = 0; i < count; ++i)
            widest = std::max(widest, measure_text(lc.font, s.node.choices[i].view(), s.scale * lc.font.base_size).x);
    } else {
        FixedString<32> a, b;
        format_to(a, "{}", static_cast<long long>(std::lround(s.node.min)));
        format_to(b, "{}", static_cast<long long>(std::lround(s.node.max)));
        widest = std::max(measure_text(lc.font, a.view(), s.scale * lc.font.base_size).x,
                          measure_text(lc.font, b.view(), s.scale * lc.font.base_size).x);
    }
    return std::max(detail::kUiValueWidth, widest + detail::kUiArrowWidth * 2.0f);
}

TextAlign block_align(const LayoutCtx &lc, const UiNode &n) {
    UiThemeClass scratch;
    return detail::ui_class_of(lc.theme, n, scratch).align;
}

// Wraps a TextBlock at `wrap` pixels (only when its text, width, font or size changed).
void wrap_block(const LayoutCtx &lc, Slot &s, float wrap) {
    UiBlockData &b = *s.block;
    const float size = s.scale * lc.font.base_size;
    // The class alignment is baked into the layout: a theme that loads or reloads with another
    // align re-wraps too.
    const TextAlign align = block_align(lc, s.node);
    if (b.laid_version == b.version && b.laid_width == wrap && b.laid_font == &lc.font && b.laid_size == size &&
        b.laid_align == align)
        return;
    b.layout.set(lc.font, b.text, size, wrap, align);
    ++lc.wraps;
    b.laid_version = b.version;
    b.laid_width = wrap;
    b.laid_font = &lc.font;
    b.laid_size = size;
    b.laid_align = align;
}

Vec2 measure(const LayoutCtx &lc, std::uint16_t i, float avail_w);

// Content size of a container's visible children.
Vec2 measure_children(const LayoutCtx &lc, Slot &s, float own_w) {
    Slots &slots = lc.slots;
    const UiNode &n = s.node;
    const float content_w = own_w > 0.0f ? std::max(0.0f, own_w - n.style.padding.x * 2) : 0.0f;
    const UiLayout layout = (n.kind == UiKind::List && n.layout == UiLayout::None) ? UiLayout::Vertical : n.layout;
    const float sp = n.style.spacing;
    const int cols = layout == UiLayout::Grid ? std::max<int>(1, n.style.grid_columns) : 1;
    const float cell_w = (layout == UiLayout::Grid && content_w > 0.0f)
                             ? std::max(0.0f, (content_w - sp * static_cast<float>(cols - 1)) / static_cast<float>(cols))
                             : 0.0f;
    Vec2 content{0, 0};
    int count = 0;
    float row_h = 0.0f, rows_h = 0.0f, cell_max_w = 0.0f;
    int rows = 0;
    for (std::uint16_t c = s.first_child; c != kUiNil; c = slots[c].next) {
        if (!slots[c].node.visible) continue;
        const UiNode &cn = slots[c].node;
        float child_avail = 0.0f;
        switch (layout) {
        case UiLayout::Vertical: child_avail = cn.kind == UiKind::Image ? 0.0f : content_w; break;
        case UiLayout::Grid: child_avail = cell_w; break;
        case UiLayout::None: child_avail = (cn.fill & kUiFillX) ? content_w : 0.0f; break;
        case UiLayout::Horizontal: break;
        }
        const Vec2 cs = measure(lc, c, child_avail);
        switch (layout) {
        case UiLayout::Vertical:
            content.x = std::max(content.x, cs.x);
            content.y += cs.y + (count > 0 ? sp : 0.0f);
            break;
        case UiLayout::Horizontal:
            content.x += cs.x + (count > 0 ? sp : 0.0f);
            content.y = std::max(content.y, cs.y);
            break;
        case UiLayout::Grid:
            cell_max_w = std::max(cell_max_w, cs.x);
            row_h = std::max(row_h, cs.y);
            if ((count + 1) % cols == 0) {
                rows_h += row_h + (rows > 0 ? sp : 0.0f);
                ++rows;
                row_h = 0.0f;
            }
            break;
        case UiLayout::None:
            content.x = std::max(content.x, std::max(0.0f, cn.rect.x) + cs.x);
            content.y = std::max(content.y, std::max(0.0f, cn.rect.y) + cs.y);
            break;
        }
        ++count;
    }
    if (layout == UiLayout::Grid && count > 0) {
        if (count % cols != 0) {
            rows_h += row_h + (rows > 0 ? sp : 0.0f);
            ++rows;
        }
        const int used_cols = std::min(count, cols);
        content.x = cell_max_w * static_cast<float>(used_cols) + sp * static_cast<float>(used_cols - 1);
        content.y = rows_h;
    }
    return content;
}

// Natural outer size of node i (cached in Slot::natural). avail_w: the width the parent will
// give it when it stretches it (0 = unknown).
Vec2 measure(const LayoutCtx &lc, std::uint16_t i, float avail_w) {
    Slot &s = lc.slots[i];
    UiNode &n = s.node;
    class_metrics(lc, s);
    const Vec2 pad = n.style.padding;
    const float lh = s.line_height;
    float own_w = n.rect.w > 0.0f ? n.rect.w : avail_w;
    if ((n.fill & kUiFillX) && avail_w > 0.0f) own_w = avail_w;
    if (n.max_size.x > 0.0f && own_w > n.max_size.x) own_w = n.max_size.x;
    Vec2 nat{0, 0};
    switch (n.kind) {
    case UiKind::Label:
    case UiKind::Button: {
        const Vec2 t = label_size(lc, s);
        nat = {t.x + pad.x * 2, std::max(t.y, lh) + pad.y * 2};
        break;
    }
    case UiKind::TextInput: {
        const Vec2 t = label_size(lc, s);
        nat = {std::max(t.x + lh * 0.75f, detail::kUiInputMinWidth) + pad.x * 2, lh + pad.y * 2};
        break;
    }
    case UiKind::Toggle:
    case UiKind::Slider:
    case UiKind::Choice: {
        const Vec2 t = label_size(lc, s);
        const float value_w = n.kind == UiKind::Toggle ? lh
                              : n.kind == UiKind::Slider ? detail::kUiValueWidth
                                                         : choice_area(lc, s);
        nat = {t.x + (t.x > 0.0f ? detail::kUiValueGap : 0.0f) + value_w + pad.x * 2, lh + pad.y * 2};
        break;
    }
    case UiKind::Image:
        nat = {n.rect.w, n.rect.h};
        break;
    case UiKind::TextBlock: {
        if (own_w > 0.0f) {
            wrap_block(lc, s, std::max(1.0f, own_w - pad.x * 2));
            nat = {own_w, s.block->layout.size().y + pad.y * 2};
        } else {
            const Vec2 t = measure_text(lc.font, s.block->text, s.scale * lc.font.base_size);
            nat = {t.x + pad.x * 2, t.y + pad.y * 2};
        }
        break;
    }
    case UiKind::Panel:
    case UiKind::List: {
        const Vec2 content = measure_children(lc, s, own_w);
        nat = {content.x + pad.x * 2, content.y + pad.y * 2};
        break;
    }
    }
    if (n.rect.w > 0.0f) nat.x = n.rect.w;
    if (n.rect.h > 0.0f) nat.y = n.rect.h;
    if (n.max_size.x > 0.0f) nat.x = std::min(nat.x, n.max_size.x);
    if (n.max_size.y > 0.0f) nat.y = std::min(nat.y, n.max_size.y);
    s.natural = nat;
    return nat;
}

void arrange(const LayoutCtx &lc, std::uint16_t i, Rect r, const Rect *clip);

// Places a child of a stack/grid/None container and recurses.
void place(const LayoutCtx &lc, std::uint16_t c, Rect r, const Rect *clip) {
    r.x = snap(r.x);
    r.y = snap(r.y);
    arrange(lc, c, r, clip);
}

// Lists: rows of `cols` children starting at the scroll row; children outside are hidden.
void arrange_list(const LayoutCtx &lc, Slot &s, Rect content, const Rect *clip) {
    Slots &slots = lc.slots;
    UiNode &n = s.node;
    const bool grid = n.layout == UiLayout::Grid;
    const int cols = grid ? std::max<int>(1, n.style.grid_columns) : 1;
    const float sp = n.style.spacing;
    const float cell_w = std::max(0.0f, (content.w - sp * static_cast<float>(cols - 1)) / static_cast<float>(cols));
    // Pass 1: total height of all rows.
    int count = 0;
    float total = 0.0f, row_h = 0.0f;
    int rows = 0;
    for (std::uint16_t c = s.first_child; c != kUiNil; c = slots[c].next) {
        if (!slots[c].node.visible) {
            hide_subtree(slots, c);
            continue;
        }
        row_h = std::max(row_h, slots[c].natural.y);
        ++count;
        if (count % cols == 0) {
            total += row_h + (rows > 0 ? sp : 0.0f);
            ++rows;
            row_h = 0.0f;
        }
    }
    if (count % cols != 0) {
        total += row_h + (rows > 0 ? sp : 0.0f);
        ++rows;
    }
    // Pass 2: the first row R whose rows R..end fit (max scroll).
    int max_row = 0;
    {
        float prefix = 0.0f;
        int k = 0, r = 0;
        row_h = 0.0f;
        if (total > content.h) {
            for (std::uint16_t c = s.first_child; c != kUiNil; c = slots[c].next) {
                if (!slots[c].node.visible) continue;
                row_h = std::max(row_h, slots[c].natural.y);
                ++k;
                if (k % cols == 0 || slots[c].next == kUiNil) {
                    prefix += row_h + sp;
                    ++r;
                    row_h = 0.0f;
                    if (total - prefix <= content.h + 0.001f) break;
                }
            }
            max_row = std::min(r, std::max(0, rows - 1));
        }
    }
    s.max_scroll = static_cast<std::uint16_t>(std::min(max_row * cols, 0xFFFF));
    int first = std::min<int>(n.scroll, s.max_scroll);
    first -= first % cols;
    n.scroll = static_cast<std::uint16_t>(first);
    // Pass 3: place.
    const Rect inner = clip ? intersection(content, *clip) : content;
    float y = content.y;
    int k = 0;
    row_h = 0.0f;
    int page_rows = 0;
    for (std::uint16_t c = s.first_child; c != kUiNil; c = slots[c].next) {
        Slot &cs = slots[c];
        if (!cs.node.visible) continue;
        const int idx = k++;
        if (idx < first) {
            hide_subtree(slots, c);
            continue;
        }
        const int col = idx % cols;
        if (col == 0 && idx > first) {
            y += row_h + sp;
            row_h = 0.0f;
        }
        if (col == 0 && y < content.y + content.h) ++page_rows;
        row_h = std::max(row_h, cs.natural.y);
        if (y >= content.y + content.h) {
            hide_subtree(slots, c);
            continue;
        }
        const float w = (cs.node.rect.w > 0.0f || cs.node.kind == UiKind::Image) ? cs.natural.x : cell_w;
        const float h = cs.natural.y;
        const float cx = content.x + static_cast<float>(col) * (cell_w + sp);
        place(lc, c, {cx + cs.node.anchor.x * (cell_w - w) + cs.node.rect.x, y + cs.node.rect.y, w, h}, &inner);
    }
    s.page_rows = static_cast<std::uint16_t>(std::max(1, page_rows));
}

void arrange_children(const LayoutCtx &lc, Slot &s, Rect content, const Rect *clip) {
    Slots &slots = lc.slots;
    const UiNode &n = s.node;
    const float sp = n.style.spacing;
    switch (n.layout) {
    case UiLayout::Vertical: {
        float y = content.y;
        for (std::uint16_t c = s.first_child; c != kUiNil; c = slots[c].next) {
            Slot &cs = slots[c];
            if (!cs.node.visible) {
                hide_subtree(slots, c);
                continue;
            }
            float w = (cs.node.rect.w > 0.0f || cs.node.kind == UiKind::Image) ? cs.natural.x : content.w;
            if (cs.node.max_size.x > 0.0f) w = std::min(w, cs.node.max_size.x);
            const float h = (cs.node.fill & kUiFillY) ? content.h : cs.natural.y;
            place(lc, c, {content.x + cs.node.anchor.x * (content.w - w) + cs.node.rect.x, y + cs.node.rect.y, w, h},
                  clip);
            y += h + sp;
        }
        break;
    }
    case UiLayout::Horizontal: {
        float x = content.x;
        for (std::uint16_t c = s.first_child; c != kUiNil; c = slots[c].next) {
            Slot &cs = slots[c];
            if (!cs.node.visible) {
                hide_subtree(slots, c);
                continue;
            }
            float h = (cs.node.rect.h > 0.0f || cs.node.kind == UiKind::Image) ? cs.natural.y : content.h;
            if (cs.node.max_size.y > 0.0f) h = std::min(h, cs.node.max_size.y);
            const float w = (cs.node.fill & kUiFillX) ? content.w : cs.natural.x;
            place(lc, c, {x + cs.node.rect.x, content.y + cs.node.anchor.y * (content.h - h) + cs.node.rect.y, w, h},
                  clip);
            x += w + sp;
        }
        break;
    }
    case UiLayout::Grid: {
        const int cols = std::max<int>(1, n.style.grid_columns);
        const float cell_w =
            std::max(0.0f, (content.w - sp * static_cast<float>(cols - 1)) / static_cast<float>(cols));
        float y = content.y, row_h = 0.0f;
        int k = 0;
        for (std::uint16_t c = s.first_child; c != kUiNil; c = slots[c].next) {
            Slot &cs = slots[c];
            if (!cs.node.visible) {
                hide_subtree(slots, c);
                continue;
            }
            const int col = k % cols;
            if (col == 0 && k > 0) {
                y += row_h + sp;
                row_h = 0.0f;
            }
            ++k;
            row_h = std::max(row_h, cs.natural.y);
            const float w = (cs.node.rect.w > 0.0f || cs.node.kind == UiKind::Image) ? cs.natural.x : cell_w;
            const float cx = content.x + static_cast<float>(col) * (cell_w + sp);
            place(lc, c, {cx + cs.node.anchor.x * (cell_w - w) + cs.node.rect.x, y + cs.node.rect.y, w, cs.natural.y},
                  clip);
        }
        break;
    }
    case UiLayout::None: {
        for (std::uint16_t c = s.first_child; c != kUiNil; c = slots[c].next) {
            Slot &cs = slots[c];
            if (!cs.node.visible) {
                hide_subtree(slots, c);
                continue;
            }
            const UiNode &cn = cs.node;
            const float w = (cn.fill & kUiFillX) ? content.w : cs.natural.x;
            const float h = (cn.fill & kUiFillY) ? content.h : cs.natural.y;
            place(lc, c,
                  {content.x + cn.anchor.x * content.w + cn.rect.x - cn.pivot.x * w,
                   content.y + cn.anchor.y * content.h + cn.rect.y - cn.pivot.y * h, w, h},
                  clip);
        }
        break;
    }
    }
}

void arrange(const LayoutCtx &lc, std::uint16_t i, Rect r, const Rect *clip) {
    Slot &s = lc.slots[i];
    UiNode &n = s.node;
    s.screen = r;
    s.shown = true;
    s.has_clip = clip != nullptr;
    if (clip) s.clip = *clip;
    s.value = {};
    const Rect content = deflate(r, n.style.padding);
    const float lh = s.line_height;
    const bool has_label = !n.text.empty();
    switch (n.kind) {
    case UiKind::Toggle:
        s.value = {content.x + content.w - lh, content.y + (content.h - lh) * 0.5f, lh, lh};
        break;
    case UiKind::Slider:
    case UiKind::Choice: {
        float w = n.kind == UiKind::Slider ? detail::kUiValueWidth : choice_area(lc, s);
        if (!has_label || w > content.w) w = content.w;
        s.value = {content.x + content.w - w, content.y + (content.h - lh) * 0.5f, w, lh};
        break;
    }
    case UiKind::TextBlock: {
        wrap_block(lc, s, std::max(1.0f, content.w));
        const int lines = s.block->layout.line_count();
        const int fit = lh > 0.0f ? static_cast<int>(std::floor((content.h + 0.001f) / lh)) : lines;
        s.max_scroll = static_cast<std::uint16_t>(std::clamp(lines - std::max(1, fit), 0, 0xFFFF));
        if (n.scroll > s.max_scroll) n.scroll = s.max_scroll;
        const Rect c = clip ? intersection(content, *clip) : content;
        s.clip = c;
        s.has_clip = true;
        s.page_rows = static_cast<std::uint16_t>(std::max(1, fit));
        break;
    }
    case UiKind::List: {
        arrange_list(lc, s, content, clip);
        return;
    }
    default:
        break;
    }
    if (s.first_child != kUiNil) arrange_children(lc, s, content, clip);
}

// Document order: parents before children, siblings in creation order.
void push_order(const Slots &slots, std::vector<std::uint16_t> &order, std::uint16_t i) {
    order.push_back(i);
    for (std::uint16_t c = slots[i].first_child; c != kUiNil; c = slots[c].next) push_order(slots, order, c);
}

} // namespace

void UiTree::layout(Rect screen, const UiTheme &theme, const Font *font) {
    Frame &f = *frame_;
    f.screen = screen;
    f.theme = &theme;
    f.font = font != nullptr ? font : &detail::ui_fallback_font();
    f.real_font = font != nullptr;
    if (order_version_ != structure_version_) {
        order_.clear();
        for (std::uint16_t r = first_root_; r != kUiNil; r = slots_[r].next) push_order(slots_, order_, r);
        order_version_ = structure_version_;
    }
    const LayoutCtx lc{slots_, theme, *f.font, measures_, wraps_};
    const auto place_roots = [&] {
        for (std::uint16_t r = first_root_; r != kUiNil; r = slots_[r].next) {
            Slot &s = slots_[r];
            if (!s.node.visible) {
                hide_subtree(slots_, r);
                continue;
            }
            const float avail = (s.node.fill & kUiFillX) ? screen.w : 0.0f;
            measure(lc, r, avail);
            const UiNode &n = s.node;
            const float w = (n.fill & kUiFillX) ? screen.w : s.natural.x;
            const float h = (n.fill & kUiFillY) ? screen.h : s.natural.y;
            const Rect rr{screen.x + n.anchor.x * screen.w + n.rect.x - n.pivot.x * w,
                          screen.y + n.anchor.y * screen.h + n.rect.y - n.pivot.y * h, w, h};
            arrange(lc, r, {std::floor(rr.x), std::floor(rr.y), rr.w, rr.h}, nullptr);
        }
    };
    place_roots();
    // A scroll_into_view asked before its list was laid out (focus() right after building a
    // menu) is done now that sizes are known, and the tree placed again.
    if (pending_scroll_ != 0) {
        const UiId id = pending_scroll_;
        pending_scroll_ = 0;
        layout_dirty_ = false;
        if (detail::ui_slot_of(slots_, id) != kUiNil) {
            scroll_into_view(id);
            if (layout_dirty_) place_roots();
        }
    }
    f.theme = nullptr;
    layout_dirty_ = false;
}

UiId UiTree::hit_test(Vec2 p) const {
    const std::uint16_t root = detail::ui_slot_of(slots_, focus_root_);
    for (std::size_t k = order_.size(); k-- > 0;) {
        const std::uint16_t i = order_[k];
        const Slot &s = slots_[i];
        if (!s.alive || !s.shown) continue;
        if (!s.screen.contains(p)) continue;
        if (s.has_clip && !s.clip.contains(p)) continue;
        if (root != kUiNil) {
            std::uint16_t a = i;
            while (a != kUiNil && a != root) a = slots_[a].parent;
            if (a == kUiNil) continue;
        }
        return detail::ui_id_of(s, i);
    }
    return 0;
}

void UiTree::scroll_by(UiId id, int rows) {
    const std::uint16_t i = detail::ui_slot_of(slots_, id);
    if (i == kUiNil) return;
    Slot &s = slots_[i];
    if (s.node.kind == UiKind::List) {
        // 64-bit: scroll_by(id, INT_MAX) ("to the end") must not overflow.
        const std::int64_t cols = s.node.layout == UiLayout::Grid ? std::max<int>(1, s.node.style.grid_columns) : 1;
        const std::int64_t next = std::clamp<std::int64_t>(std::int64_t{s.node.scroll} + std::int64_t{rows} * cols, 0,
                                                           std::int64_t{s.max_scroll});
        s.node.scroll = static_cast<std::uint16_t>(next - next % cols);
    } else if (s.node.kind == UiKind::TextBlock) {
        s.node.scroll = static_cast<std::uint16_t>(
            std::clamp<std::int64_t>(std::int64_t{s.node.scroll} + rows, 0, std::int64_t{s.max_scroll}));
    } else {
        return;
    }
    layout_dirty_ = true;
}

void UiTree::scroll_into_view(UiId id) {
    std::uint16_t child = detail::ui_slot_of(slots_, id);
    if (child == kUiNil) return;
    for (std::uint16_t p = slots_[child].parent; p != kUiNil; child = p, p = slots_[p].parent) {
        Slot &list = slots_[p];
        if (list.node.kind != UiKind::List) continue;
        if (!list.shown || list.screen.h <= 0.0f) {
            // Not laid out yet: sizes are unknown, so the next layout() does it.
            pending_scroll_ = id;
            continue;
        }
        const int cols = list.node.layout == UiLayout::Grid ? std::max<int>(1, list.node.style.grid_columns) : 1;
        // Index of `child` among the visible children.
        int idx = -1, k = 0;
        for (std::uint16_t c = list.first_child; c != kUiNil; c = slots_[c].next) {
            if (!slots_[c].node.visible) continue;
            if (c == child) {
                idx = k;
                break;
            }
            ++k;
        }
        if (idx < 0) continue;
        const int row = idx / cols;
        int first_row = list.node.scroll / cols;
        if (row < first_row) {
            first_row = row;
        } else {
            // Advance the first row until rows first_row..row fit in the content height.
            const float content_h = std::max(0.0f, list.screen.h - list.node.style.padding.y * 2);
            const float sp = list.node.style.spacing;
            for (;;) {
                float h = 0.0f, row_h = 0.0f;
                int kk = 0, r = first_row;
                bool started = false;
                for (std::uint16_t c = list.first_child; c != kUiNil; c = slots_[c].next) {
                    if (!slots_[c].node.visible) continue;
                    const int rr = kk++ / cols;
                    if (rr < first_row) continue;
                    if (rr > row) break;
                    if (rr != r) {
                        h += row_h + (started ? sp : 0.0f);
                        started = true;
                        row_h = 0.0f;
                        r = rr;
                    }
                    row_h = std::max(row_h, slots_[c].natural.y);
                }
                h += row_h + (started ? sp : 0.0f);
                if (h <= content_h + 0.001f || first_row >= row || content_h <= 0.0f) break;
                ++first_row;
            }
        }
        const int next = first_row * cols;
        if (next != list.node.scroll) {
            list.node.scroll = static_cast<std::uint16_t>(next);
            layout_dirty_ = true;
        }
    }
}

} // namespace lam
