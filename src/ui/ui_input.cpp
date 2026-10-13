// UiTree input, following the old MenuModel rules: navigation skips labels, disabled and hidden
// nodes and wraps; left/right change values; hover focuses only when the pointer moves.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <lam/core/text.h>
#include <lam/ui/ui.h>

#include "detail/ui_tree_impl.h"

namespace lam {
namespace {

using Slot = UiTree::Slot;
using Slots = std::vector<UiTree::Slot>;

bool is_value_widget(UiKind k) {
    return k == UiKind::Toggle || k == UiKind::Slider || k == UiKind::Choice;
}

bool in_subtree(const Slots &slots, std::uint16_t i, std::uint16_t root) {
    if (root == kUiNil) return true;
    for (std::uint16_t a = i; a != kUiNil; a = slots[a].parent)
        if (a == root) return true;
    return false;
}

// Focus candidates: alive, visible chain, enabled, focusable, not a label, inside the focus root.
bool can_focus(const Slots &slots, std::uint16_t i, std::uint16_t root) {
    const Slot &s = slots[i];
    return s.alive && s.node.enabled && s.node.focusable && s.node.kind != UiKind::Label &&
           detail::ui_visible_chain(slots, i) && in_subtree(slots, i, root);
}

float slider_step(const UiNode &n) {
    return n.step > 0.0f ? n.step : (n.max - n.min) / 10.0f;
}

// Snaps to the step grid that starts at min (no float drift after many presses).
float snap_slider(const UiNode &n, float v) {
    const float step = slider_step(n);
    v = std::clamp(v, n.min, n.max);
    if (step > 0.0f) {
        v = n.min + std::round((v - n.min) / step) * step;
        v = std::clamp(v, n.min, n.max);
    }
    return v;
}

} // namespace

struct UiInputOps {
    UiTree &t;
    Slots &slots;
    std::uint16_t root;                                         // focus root slot (kUiNil = whole tree)

    UiId id_of(std::uint16_t i) const { return i == kUiNil ? 0 : detail::ui_id_of(slots[i], i); }

    void emit(UiEvent::Kind kind, std::uint16_t i) {
        UiEvent e;
        e.kind = kind;
        if (i != kUiNil) {
            e.node = id_of(i);
            e.id = slots[i].node.id;
            e.value = slots[i].node.value;
        }
        if (t.events_.size() < kUiMaxEvents) t.events_.push_back(e);
        else ++t.dropped_events_;
    }

    void set_focus(std::uint16_t i, bool user) {
        const UiId id = id_of(i);
        if (id == t.focused_) return;
        t.focused_ = id;
        if (i != kUiNil) t.scroll_into_view(id);
        t.layout_dirty_ = true;
        if (user && i != kUiNil) emit(UiEvent::Kind::Focused, i);
    }

    // Position of slot i in document order (order_ is rebuilt by layout()).
    int order_pos(std::uint16_t i) const {
        const auto &order = t.order_;
        for (std::size_t k = 0; k < order.size(); ++k)
            if (order[k] == i) return static_cast<int>(k);
        return -1;
    }

    // Next focusable in document order from `from` (kUiNil = before the first), wrapping;
    // skips the subtree `skip` (kUiNil = none). kUiNil if there is none.
    std::uint16_t next_in_order(std::uint16_t from, int dir, std::uint16_t skip) const {
        const auto &order = t.order_;
        const int n = static_cast<int>(order.size());
        if (n == 0) return kUiNil;
        int pos = from == kUiNil ? (dir > 0 ? -1 : n) : order_pos(from);
        if (pos < 0 && from != kUiNil) pos = dir > 0 ? -1 : n;
        for (int step = 0; step < n; ++step) {
            pos += dir;
            if (pos >= n) pos = 0;
            if (pos < 0) pos = n - 1;
            const std::uint16_t i = order[static_cast<std::size_t>(pos)];
            if (skip != kUiNil && in_subtree(slots, i, skip)) continue;
            if (can_focus(slots, i, root)) return i;
        }
        return kUiNil;
    }

    // First focusable in the subtree of `cell` (itself included); kUiNil if none.
    std::uint16_t first_focusable_in(std::uint16_t cell, int dir) const {
        if (dir > 0) {
            if (can_focus(slots, cell, root)) return cell;
            for (std::uint16_t c = slots[cell].first_child; c != kUiNil; c = slots[c].next) {
                const std::uint16_t f = first_focusable_in(c, dir);
                if (f != kUiNil) return f;
            }
            return kUiNil;
        }
        for (std::uint16_t c = slots[cell].last_child; c != kUiNil; c = slots[c].prev) {
            const std::uint16_t f = first_focusable_in(c, dir);
            if (f != kUiNil) return f;
        }
        return can_focus(slots, cell, root) ? cell : kUiNil;
    }

    // Nearest ancestor laid out as a row or Grid, and its child that holds `i`.
    bool find_row(std::uint16_t i, std::uint16_t &row, std::uint16_t &cell) const {
        std::uint16_t c = i;
        for (std::uint16_t p = slots[i].parent; p != kUiNil; c = p, p = slots[p].parent) {
            if (root != kUiNil && !in_subtree(slots, p, root)) return false;
            const UiLayout l = slots[p].node.layout;
            if (l == UiLayout::Horizontal || l == UiLayout::Grid) {
                row = p;
                cell = c;
                return true;
            }
            if (p == root) return false;
        }
        return false;
    }

    // Visible cells of `row` in order: index of `cell` and the count.
    void cell_index(std::uint16_t row, std::uint16_t cell, int &index, int &count) const {
        index = -1;
        count = 0;
        for (std::uint16_t c = slots[row].first_child; c != kUiNil; c = slots[c].next) {
            if (!slots[c].node.visible) continue;
            if (c == cell) index = count;
            ++count;
        }
    }

    std::uint16_t cell_at(std::uint16_t row, int index) const {
        int k = 0;
        for (std::uint16_t c = slots[row].first_child; c != kUiNil; c = slots[c].next) {
            if (!slots[c].node.visible) continue;
            if (k++ == index) return c;
        }
        return kUiNil;
    }

    // Moves inside a row/grid by `delta` cells, wrapping; kUiNil if no other cell can take focus.
    // Entering a cell picks its focusable nearest to `from` vertically (side-by-side columns).
    std::uint16_t step_cells(std::uint16_t row, int index, int count, int delta, bool wrap,
                             std::uint16_t from) const {
        int k = index;
        for (int step = 0; step < count; ++step) {
            k += delta;
            if (k < 0 || k >= count) {
                if (!wrap) return kUiNil;
                k = ((k % count) + count) % count;
            }
            if (k == index) break;
            const std::uint16_t f = nearest_in(cell_at(row, k), from, delta > 0 ? 1 : -1);
            if (f != kUiNil) return f;
        }
        return kUiNil;
    }

    // The focusable in the subtree of `cell` whose centre is vertically nearest to `from`'s
    // (shown nodes only; ties: document order); else the first focusable in direction `dir`.
    std::uint16_t nearest_in(std::uint16_t cell, std::uint16_t from, int dir) const {
        const int first = order_pos(cell), last = order_pos(last_in_subtree(cell));
        if (first < 0 || last < 0 || !slots[from].shown) return first_focusable_in(cell, dir);
        const float y = slots[from].screen.y + slots[from].screen.h * 0.5f;
        std::uint16_t best = kUiNil;
        float best_d = 0.0f;
        for (int k = first; k <= last; ++k) {
            const std::uint16_t i = t.order_[static_cast<std::size_t>(k)];
            if (!slots[i].shown || !can_focus(slots, i, root)) continue;
            const float d = std::fabs(slots[i].screen.y + slots[i].screen.h * 0.5f - y);
            if (best == kUiNil || d < best_d) {
                best = i;
                best_d = d;
            }
        }
        return best != kUiNil ? best : first_focusable_in(cell, dir);
    }

    // The next focusable inside container `a`, after the subtree of its descendant `child`
    // (dir > 0) or before it (dir < 0), in document order without wrapping; kUiNil if none.
    std::uint16_t next_in_container(std::uint16_t a, std::uint16_t child, int dir) const {
        const int a_first = order_pos(a), a_last = order_pos(last_in_subtree(a));
        const int c_first = order_pos(child), c_last = order_pos(last_in_subtree(child));
        if (a_first < 0 || a_last < 0 || c_first < 0 || c_last < 0) return kUiNil;
        if (dir > 0) {
            for (int k = c_last + 1; k <= a_last; ++k) {
                const std::uint16_t i = t.order_[static_cast<std::size_t>(k)];
                if (can_focus(slots, i, root)) return i;
            }
        } else {
            for (int k = c_first - 1; k >= a_first; --k) {
                const std::uint16_t i = t.order_[static_cast<std::size_t>(k)];
                if (can_focus(slots, i, root)) return i;
            }
        }
        return kUiNil;
    }

    // Column move inside grid `g` from its child `cell` (wrap: to the other end of the column).
    std::uint16_t grid_column(std::uint16_t g, std::uint16_t cell, int dir, bool wrap) const {
        int index, count;
        cell_index(g, cell, index, count);
        if (index < 0) return kUiNil;
        const int cols = std::max<int>(1, slots[g].node.style.grid_columns);
        if (!wrap) {
            for (int k = index + dir * cols; k >= 0 && k < count; k += dir * cols) {
                const std::uint16_t f = first_focusable_in(cell_at(g, k), dir);
                if (f != kUiNil) return f;
            }
            return kUiNil;
        }
        const int col = index % cols;
        for (int k = dir > 0 ? col : col + cols * ((count - 1 - col) / cols); k >= 0 && k < count && k != index;
             k += dir * cols) {
            const std::uint16_t f = first_focusable_in(cell_at(g, k), dir);
            if (f != kUiNil) return f;
        }
        return kUiNil;
    }

    static UiLayout axis_layout(const UiNode &n) {
        return (n.kind == UiKind::List && n.layout == UiLayout::None) ? UiLayout::Vertical : n.layout;
    }

    // Up/down: a vertical stack moves to its next focusable, a grid by a column, and a row is
    // left along the outer column. At the end, wrap inside the outermost stack or grid crossed,
    // else in document order over the whole tree.
    std::uint16_t vertical_target(std::uint16_t cur, int dir) const {
        std::uint16_t child = cur, outer = kUiNil, outer_child = kUiNil, row = kUiNil;
        for (std::uint16_t a = slots[cur].parent; a != kUiNil; child = a, a = slots[a].parent) {
            if (root != kUiNil && !in_subtree(slots, a, root)) break;
            switch (axis_layout(slots[a].node)) {
            case UiLayout::Grid:
                if (const std::uint16_t f = grid_column(a, child, dir, false); f != kUiNil) return f;
                outer = a;
                outer_child = child;
                break;
            case UiLayout::Vertical:
                if (const std::uint16_t f = next_in_container(a, child, dir); f != kUiNil) return f;
                outer = a;
                outer_child = child;
                break;
            case UiLayout::Horizontal:
                row = a;
                break;
            case UiLayout::None:
                break;
            }
            if (a == root) break;
        }
        std::uint16_t f = kUiNil;
        if (outer != kUiNil && axis_layout(slots[outer].node) == UiLayout::Grid) {
            f = grid_column(outer, outer_child, dir, true);
        } else if (outer != kUiNil) {
            f = first_focusable_in(outer, dir);
        } else {
            const std::uint16_t anchor = row == kUiNil ? cur : (dir > 0 ? last_in_subtree(row) : row);
            f = next_in_order(anchor, dir, row);
        }
        return f == cur ? kUiNil : f;
    }

    std::uint16_t last_in_subtree(std::uint16_t i) const {
        while (slots[i].last_child != kUiNil) i = slots[i].last_child;
        return i;
    }

    // Left/right for non-value widgets inside a row or grid.
    std::uint16_t horizontal_target(std::uint16_t cur, int dir) const {
        std::uint16_t row, cell;
        if (!find_row(cur, row, cell)) return kUiNil;
        int index, count;
        cell_index(row, cell, index, count);
        return step_cells(row, index, count, dir, true, cur);
    }

    // Changes a value widget by `dir`; Changed when the value changed.
    void step_value(std::uint16_t i, int dir) {
        UiNode &n = slots[i].node;
        const float before = n.value;
        switch (n.kind) {
        case UiKind::Toggle:
            n.value = n.value >= 0.5f ? 0.0f : 1.0f;
            break;
        case UiKind::Slider:
            n.value = snap_slider(n, n.value + slider_step(n) * static_cast<float>(dir));
            break;
        case UiKind::Choice: {
            if (n.choice_count > 0) {
                // choice_count can be edited through get(): clamp it at every use.
                const int count = std::min<int>(n.choice_count, kMaxUiChoices);
                if (count < 2) return;
                const int v = std::clamp(static_cast<int>(std::lround(n.value)), 0, count - 1);
                n.value = static_cast<float>(((v + dir) % count + count) % count);
            } else {
                // 64-bit, from bounds held to +-1e15, so a wide range neither overflows nor
                // narrows.
                const std::int64_t lo = std::llround(std::clamp(n.min, -1e15f, 1e15f));
                const std::int64_t hi = std::llround(std::clamp(n.max, -1e15f, 1e15f));
                const std::int64_t count = hi - lo + 1;
                if (count < 2) return;
                const std::int64_t v = std::clamp<std::int64_t>(std::llround(std::clamp(n.value, -1e15f, 1e15f)), lo, hi);
                n.value = static_cast<float>(lo + ((v - lo + dir) % count + count) % count);
            }
            break;
        }
        default:
            return;
        }
        if (n.value != before) {
            t.layout_dirty_ = true;
            emit(UiEvent::Kind::Changed, i);
        }
    }

    // Confirm (or a click) on node i.
    void activate(std::uint16_t i, int choice_dir) {
        UiNode &n = slots[i].node;
        if (!n.enabled) return;
        switch (n.kind) {
        case UiKind::Toggle: step_value(i, 1); break;
        case UiKind::Choice: step_value(i, choice_dir); break;
        case UiKind::Slider: break;
        case UiKind::TextInput: emit(UiEvent::Kind::Submitted, i); break;
        default: emit(UiEvent::Kind::Clicked, i); break;
        }
    }

    void slider_from_pointer(std::uint16_t i, float x) {
        UiNode &n = slots[i].node;
        const Rect bar = slots[i].value;
        if (bar.w <= 0.0f) return;
        const float f = std::clamp((x - bar.x) / bar.w, 0.0f, 1.0f);
        const float v = snap_slider(n, n.min + f * (n.max - n.min));
        if (v != n.value) {
            n.value = v;
            t.layout_dirty_ = true;
            emit(UiEvent::Kind::Changed, i);
        }
    }

    // Nearest focusable, enabled ancestor-or-self of a hit node.
    std::uint16_t focus_target(std::uint16_t i) const {
        for (std::uint16_t a = i; a != kUiNil; a = slots[a].parent) {
            const Slot &s = slots[a];
            if (s.node.focusable && s.node.kind != UiKind::Label) return can_focus(slots, a, root) ? a : kUiNil;
        }
        return kUiNil;
    }

    std::uint16_t list_ancestor(std::uint16_t i) const {
        for (std::uint16_t a = i; a != kUiNil; a = slots[a].parent) {
            const UiKind k = slots[a].node.kind;
            if ((k == UiKind::List || k == UiKind::TextBlock) && slots[a].max_scroll > 0) return a;
        }
        return kUiNil;
    }

    void keys(const UiInput &in) {
        std::uint16_t cur = detail::ui_slot_of(slots, t.focused_);
        if (cur != kUiNil && !can_focus(slots, cur, root)) cur = kUiNil;   // hidden/disabled since
        if (cur == kUiNil && t.focused_ != 0) t.focused_ = 0;
        if (in.back) emit(UiEvent::Kind::Back, cur);
        const bool any_nav = in.up || in.down || in.left || in.right || in.confirm;
        if (cur == kUiNil) {
            // Nothing focused: the first navigation key only focuses (up: the last item).
            if (!any_nav) return;
            const std::uint16_t f = next_in_order(kUiNil, in.up && !in.down ? -1 : 1, kUiNil);
            if (f != kUiNil) set_focus(f, true);
            return;
        }
        UiNode &n = slots[cur].node;
        if (n.kind == UiKind::TextInput) {
            // Direction keys stay in the field (WASD types letters). Confirm submits and moves
            // on, like a form. A space typed this frame is text, not confirm.
            text_input(cur, in);
            const bool typed_space = std::find(in.typed.begin(), in.typed.end(), U' ') != in.typed.end();
            if (in.confirm && !typed_space) {
                activate(cur, 1);
                const std::uint16_t f = vertical_target(cur, 1);
                if (f != kUiNil && f != cur) set_focus(f, true);
            }
            return;
        }
        if (in.up != in.down) {
            const std::uint16_t f = vertical_target(cur, in.down ? 1 : -1);
            if (f != kUiNil && f != cur) set_focus(f, true);
            return;
        }
        if (in.left != in.right) {
            const int dir = in.right ? 1 : -1;
            if (is_value_widget(n.kind)) {
                step_value(cur, dir);
            } else {
                const std::uint16_t f = horizontal_target(cur, dir);
                if (f != kUiNil && f != cur) set_focus(f, true);
            }
            return;
        }
        if (in.confirm) activate(cur, 1);
    }

    void text_input(std::uint16_t i, const UiInput &in) {
        UiNode &n = slots[i].node;
        bool changed = false;
        if (in.erase && !n.text.empty()) {
            // Drop the last code point.
            const std::string_view v = n.text.view();
            std::size_t cut = v.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(v[cut]) & 0xC0u) == 0x80u) --cut;
            n.text.set_size(cut);
            changed = true;
        }
        for (char32_t cp : in.typed) {
            if (cp < 32 || cp == 127 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) continue;
            char buf[4];
            std::size_t len;
            if (cp < 0x80) {
                buf[0] = static_cast<char>(cp);
                len = 1;
            } else if (cp < 0x800) {
                buf[0] = static_cast<char>(0xC0 | (cp >> 6));
                buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
                len = 2;
            } else if (cp < 0x10000) {
                buf[0] = static_cast<char>(0xE0 | (cp >> 12));
                buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
                len = 3;
            } else {
                buf[0] = static_cast<char>(0xF0 | (cp >> 18));
                buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
                len = 4;
            }
            const std::size_t size = n.text.size();
            if (size + len > n.text.capacity()) break;          // full: the rest is dropped
            for (std::size_t k = 0; k < len; ++k) n.text.buffer()[size + k] = buf[k];
            n.text.set_size(size + len);
            changed = true;
        }
        if (changed) {
            t.layout_dirty_ = true;
            emit(UiEvent::Kind::Changed, i);
        }
    }

    void pointer(const UiInput &in) {
        if (!in.pointer_valid) {
            t.hovered_ = 0;
            if (!in.pointer_down) t.pressed_ = 0;
            return;
        }
        const std::uint16_t hit = detail::ui_slot_of(slots, t.hit_test(in.pointer));
        const std::uint16_t target = hit == kUiNil ? kUiNil : focus_target(hit);
        t.hovered_ = id_of(target);
        // The first pointer position seen is not a move: a resting mouse does not steal the
        // initial focus of a menu that just opened.
        const bool seen = t.last_pointer_.x > -1e8f;
        const bool moved = seen && (in.pointer.x != t.last_pointer_.x || in.pointer.y != t.last_pointer_.y);
        t.last_pointer_ = in.pointer;
        std::uint16_t pressed = detail::ui_slot_of(slots, t.pressed_);
        if (in.pointer_pressed) {
            pressed = target;
            t.pressed_ = id_of(target);
            if (target != kUiNil) {
                set_focus(target, true);
                if (slots[target].node.kind == UiKind::Slider) slider_from_pointer(target, in.pointer.x);
            }
        } else if (moved && target != kUiNil && pressed == kUiNil) {
            set_focus(target, true);                            // hover selects (only when moving)
        }
        if (pressed != kUiNil && in.pointer_down && !in.pointer_pressed &&
            slots[pressed].node.kind == UiKind::Slider && slots[pressed].node.enabled) {
            slider_from_pointer(pressed, in.pointer.x);         // drag
        }
        if (in.pointer_released && pressed != kUiNil) {
            if (pressed == target) {
                int dir = 1;
                if (slots[pressed].node.kind == UiKind::Choice) {
                    const Rect v = slots[pressed].value;
                    if (in.pointer.x < v.x + v.w * 0.5f) dir = -1;   // "<" half steps back
                }
                activate(pressed, dir);
            }
            t.pressed_ = 0;
        } else if (!in.pointer_down && !in.pointer_pressed) {
            t.pressed_ = 0;
        }
        if (in.wheel != 0.0f && hit != kUiNil) {
            const std::uint16_t list = list_ancestor(hit);
            if (list != kUiNil) {
                const int rows = in.wheel > 0.0f ? -static_cast<int>(std::ceil(in.wheel))
                                                 : static_cast<int>(std::ceil(-in.wheel));
                t.scroll_by(id_of(list), rows);
            }
        }
    }
};

void UiTree::handle_input(const UiInput &in) {
    UiInputOps ops{*this, slots_, detail::ui_slot_of(slots_, focus_root_)};
    ops.keys(in);
    ops.pointer(in);
}

UiInput ui_input_from(const FrameInput &frame, const UiNav &nav) {
    UiInput in;
    in.up = frame.is_repeated(nav.up) || frame.is_pressed(nav.up);
    in.down = frame.is_repeated(nav.down) || frame.is_pressed(nav.down);
    in.left = frame.is_repeated(nav.left) || frame.is_pressed(nav.left);
    in.right = frame.is_repeated(nav.right) || frame.is_pressed(nav.right);
    in.confirm = frame.is_pressed(nav.confirm);
    in.back = frame.is_pressed(nav.back);
    in.erase = frame.is_repeated(nav.erase) || frame.is_pressed(nav.erase);
    const PointerState &p = frame.pointer;
    in.pointer = p.virtual_pos;
    in.pointer_valid = p.valid;
    in.pointer_down = (p.down & 1u) != 0;
    in.pointer_pressed = (p.pressed & 1u) != 0;
    in.pointer_released = (p.released & 1u) != 0;
    in.wheel = p.wheel;
    return in;
}

} // namespace lam
