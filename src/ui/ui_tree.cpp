// UiTree storage: generational slots, child links, builders, text/value setters and
// focus. Layout lives in ui_layout.cpp, input in ui_input.cpp, drawing in ui_draw.cpp.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <lam/core/assert.h>
#include <lam/core/text.h>
#include <lam/ui/ui.h>

#include "detail/ui_tree_impl.h"

namespace lam {
namespace detail {

std::uint16_t ui_slot_of(const std::vector<UiTree::Slot> &slots, UiId id) {
    if (id == 0) return kUiNil;
    const std::uint32_t index = id & 0xFFFFu;
    const std::uint32_t gen = id >> 16;
    if (index >= slots.size()) return kUiNil;
    const UiTree::Slot &s = slots[index];
    if (!s.alive || s.generation != gen) return kUiNil;
    return static_cast<std::uint16_t>(index);
}

UiId ui_id_of(const UiTree::Slot &s, std::uint16_t index) {
    return (static_cast<UiId>(s.generation) << 16) | index;
}

bool ui_visible_chain(const std::vector<UiTree::Slot> &slots, std::uint16_t index) {
    while (index != kUiNil) {
        const UiTree::Slot &s = slots[index];
        if (!s.node.visible) return false;
        index = s.parent;
    }
    return true;
}

} // namespace detail

namespace {

bool interactive(UiKind k) {
    return k == UiKind::Button || k == UiKind::Toggle || k == UiKind::Slider || k == UiKind::Choice ||
           k == UiKind::TextInput;
}

float finite_or(float v, float fallback) {
    return std::isfinite(v) ? v : fallback;
}

// Every value is finite and in range; choice indices are always valid.
void sanitize(UiNode &n) {
    n.min = finite_or(n.min, 0.0f);
    n.max = finite_or(n.max, 1.0f);
    if (n.max < n.min) std::swap(n.min, n.max);
    n.step = finite_or(n.step, 0.0f);
    if (n.step < 0.0f) n.step = -n.step;
    n.value = finite_or(n.value, n.min);
    if (n.choice_count > kMaxUiChoices) n.choice_count = kMaxUiChoices;
    switch (n.kind) {
    case UiKind::Toggle:
        n.value = n.value >= 0.5f ? 1.0f : 0.0f;
        break;
    case UiKind::Slider:
        n.value = std::clamp(n.value, n.min, n.max);
        break;
    case UiKind::Choice:
        if (n.choice_count > 0) {
            n.value = std::clamp(std::round(n.value), 0.0f, static_cast<float>(n.choice_count - 1));
        } else {
            n.min = std::round(n.min);
            n.max = std::round(n.max);
            n.value = std::clamp(std::round(n.value), n.min, n.max);
        }
        break;
    default:
        break;
    }
    if (!std::isfinite(n.rect.x)) n.rect.x = 0;
    if (!std::isfinite(n.rect.y)) n.rect.y = 0;
    if (!std::isfinite(n.rect.w) || n.rect.w < 0) n.rect.w = 0;
    if (!std::isfinite(n.rect.h) || n.rect.h < 0) n.rect.h = 0;
    if (n.style.grid_columns == 0) n.style.grid_columns = 1;
}

} // namespace

UiTree::UiTree() : frame_(std::make_unique<Frame>()) {
    events_.reserve(kUiMaxEvents);
}

UiTree::~UiTree() = default;

bool UiTree::alive(UiId id) const {
    return detail::ui_slot_of(slots_, id) != kUiNil;
}

UiId UiTree::add(UiId parent, const UiNode &node) {
    // `node` may live in this tree (t.add(p, *t.get(template))): copy it before the slot store
    // can grow and free it.
    const UiNode n = node;
    std::uint16_t parent_index = kUiNil;
    if (parent != 0) {
        parent_index = detail::ui_slot_of(slots_, parent);
        LAM_ASSERT(parent_index != kUiNil, "UiTree::add: parent is not a live node");
        if (parent_index == kUiNil) return 0;
    }
    std::uint16_t index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        LAM_CHECK(slots_.size() < kUiMaxSlots, "UiTree::add: more than 65535 nodes");
        index = static_cast<std::uint16_t>(slots_.size());
        slots_.emplace_back();
        if (order_.capacity() < slots_.capacity()) order_.reserve(slots_.capacity());
        if (free_.capacity() < slots_.capacity()) free_.reserve(slots_.capacity());
    }
    Slot &s = slots_[index];
    s.alive = true;
    s.generation = static_cast<std::uint16_t>(s.generation + 1);   // 1 on first use
    s.node = n;
    s.node.parent = parent;
    if (interactive(s.node.kind)) s.node.focusable = true;
    sanitize(s.node);
    s.parent = parent_index;
    s.first_child = s.last_child = s.next = s.prev = kUiNil;
    s.shown = false;
    s.has_clip = false;
    s.screen = s.clip = s.value = {};
    s.natural = {};
    s.max_scroll = 0;
    s.measured_font = nullptr;
    s.measured_scale = -1.0f;
    s.block.reset();
    if (s.node.kind == UiKind::TextBlock) s.block = std::make_unique<UiBlockData>();
    // Link as the last child.
    std::uint16_t &first = parent_index == kUiNil ? first_root_ : slots_[parent_index].first_child;
    std::uint16_t &last = parent_index == kUiNil ? last_root_ : slots_[parent_index].last_child;
    s.prev = last;
    if (last != kUiNil) slots_[last].next = index;
    else first = index;
    last = index;
    ++live_;
    ++structure_version_;
    layout_dirty_ = true;
    return detail::ui_id_of(s, index);
}

UiId UiTree::panel(UiId parent, Rect r, UiLayout l) {
    UiNode n;
    n.kind = UiKind::Panel;
    n.rect = r;
    n.layout = l;
    return add(parent, n);
}

UiId UiTree::label(UiId parent, std::string_view text) {
    UiNode n;
    n.kind = UiKind::Label;
    n.text = text;
    return add(parent, n);
}

UiId UiTree::button(UiId parent, StrId id, std::string_view label) {
    UiNode n;
    n.kind = UiKind::Button;
    n.id = id;
    n.text = label;
    return add(parent, n);
}

UiId UiTree::toggle(UiId parent, StrId id, std::string_view label, bool on) {
    UiNode n;
    n.kind = UiKind::Toggle;
    n.id = id;
    n.text = label;
    n.value = on ? 1.0f : 0.0f;
    return add(parent, n);
}

UiId UiTree::slider(UiId parent, StrId id, std::string_view label, float v, float mn, float mx, float step) {
    UiNode n;
    n.kind = UiKind::Slider;
    n.id = id;
    n.text = label;
    n.min = mn;
    n.max = mx;
    n.step = step;
    n.value = v;
    return add(parent, n);
}

UiId UiTree::choice(UiId parent, StrId id, std::string_view label, std::span<const std::string_view> choices,
                    int selected) {
    UiNode n;
    n.kind = UiKind::Choice;
    n.id = id;
    n.text = label;
    const std::size_t count = std::min<std::size_t>(choices.size(), kMaxUiChoices);
    for (std::size_t i = 0; i < count; ++i) n.choices[i] = choices[i];
    n.choice_count = static_cast<std::uint8_t>(count);
    n.value = static_cast<float>(selected);
    if (count == 0) n.min = n.max = 0;
    return add(parent, n);
}

UiId UiTree::list(UiId parent, Rect r) {
    UiNode n;
    n.kind = UiKind::List;
    n.rect = r;
    n.layout = UiLayout::Vertical;
    return add(parent, n);
}

UiId UiTree::grid(UiId parent, Rect r, int columns) {
    UiNode n;
    n.kind = UiKind::List;
    n.rect = r;
    n.layout = UiLayout::Grid;
    n.style.grid_columns = static_cast<std::uint8_t>(std::clamp(columns, 1, 255));
    return add(parent, n);
}

UiId UiTree::text_block(UiId parent, Rect r, std::string text) {
    UiNode n;
    n.kind = UiKind::TextBlock;
    n.rect = r;
    const UiId id = add(parent, n);
    if (id != 0) set_block_text(id, std::move(text));
    return id;
}

UiId UiTree::image(UiId parent, const TextureRegion &region, Vec2 size, StrId id) {
    UiNode n;
    n.kind = UiKind::Image;
    n.image = region;
    const Vec2 natural = (region.size.x > 0 && region.size.y > 0) ? region.size
                                                                  : Vec2{region.source.w, region.source.h};
    n.rect.w = size.x > 0 ? size.x : natural.x;
    n.rect.h = size.y > 0 ? size.y : natural.y;
    n.id = id;
    n.focusable = static_cast<bool>(id);
    return add(parent, n);
}

UiId UiTree::text_input(UiId parent, StrId id, std::string_view initial) {
    UiNode n;
    n.kind = UiKind::TextInput;
    n.id = id;
    n.text = initial;
    return add(parent, n);
}

void UiTree::remove(UiId id) {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    if (index == kUiNil) return;
    // Remove children first (deepest last-child first keeps the links simple).
    while (slots_[index].first_child != kUiNil) {
        const std::uint16_t c = slots_[index].first_child;
        remove(detail::ui_id_of(slots_[c], c));
    }
    Slot &s = slots_[index];
    std::uint16_t &first = s.parent == kUiNil ? first_root_ : slots_[s.parent].first_child;
    std::uint16_t &last = s.parent == kUiNil ? last_root_ : slots_[s.parent].last_child;
    if (s.prev != kUiNil) slots_[s.prev].next = s.next;
    else first = s.next;
    if (s.next != kUiNil) slots_[s.next].prev = s.prev;
    else last = s.prev;
    if (focused_ == id) focused_ = 0;
    if (hovered_ == id) hovered_ = 0;
    if (pressed_ == id) pressed_ = 0;
    if (focus_root_ == id) focus_root_ = 0;
    s.alive = false;
    s.shown = false;
    s.block.reset();
    s.parent = s.first_child = s.last_child = s.next = s.prev = kUiNil;
    if (s.generation == 0xFFFFu) s.retired = true;              // never reuse an exhausted slot
    else free_.push_back(index);
    --live_;
    ++structure_version_;
    layout_dirty_ = true;
}

UiNode *UiTree::get(UiId id) {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    return index == kUiNil ? nullptr : &slots_[index].node;
}

const UiNode *UiTree::get(UiId id) const {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    return index == kUiNil ? nullptr : &slots_[index].node;
}

UiId UiTree::find(StrId id) const {
    if (!id) return 0;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const Slot &s = slots_[i];
        if (s.alive && s.node.id == id) return detail::ui_id_of(s, static_cast<std::uint16_t>(i));
    }
    return 0;
}

void UiTree::set_text(UiId id, std::string_view text) {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    if (index == kUiNil) return;
    Slot &s = slots_[index];
    if (s.node.kind == UiKind::TextBlock && s.block) {
        s.block->text.assign(text);
        ++s.block->version;
    } else {
        if (s.node.text.view() == text) return;
        s.node.text = text;
    }
    layout_dirty_ = true;
}

void UiTree::set_block_text(UiId id, std::string text) {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    if (index == kUiNil) return;
    Slot &s = slots_[index];
    if (!s.block) return;
    s.block->text = std::move(text);
    ++s.block->version;
    s.node.scroll = 0;
    layout_dirty_ = true;
}

std::string_view UiTree::block_text(UiId id) const {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    if (index == kUiNil || !slots_[index].block) return {};
    return slots_[index].block->text;
}

void UiTree::set_reveal(UiId id, int glyphs) {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    if (index == kUiNil || !slots_[index].block) return;
    slots_[index].block->reveal = glyphs < 0 ? -1 : glyphs;
}

void UiTree::set_value(UiId id, float value) {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    if (index == kUiNil) return;
    UiNode &n = slots_[index].node;
    n.value = value;
    sanitize(n);
    layout_dirty_ = true;
}

void UiTree::set_choices(UiId id, std::span<const std::string_view> choices, int selected) {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    if (index == kUiNil) return;
    UiNode &n = slots_[index].node;
    const std::size_t count = std::min<std::size_t>(choices.size(), kMaxUiChoices);
    for (std::size_t i = 0; i < kMaxUiChoices; ++i) {
        if (i < count) n.choices[i] = choices[i];
        else n.choices[i].clear();
    }
    n.choice_count = static_cast<std::uint8_t>(count);
    n.value = static_cast<float>(selected);
    sanitize(n);
    layout_dirty_ = true;
}

void UiTree::focus(UiId id) {
    if (id != 0 && detail::ui_slot_of(slots_, id) == kUiNil) return;
    focused_ = id;
    if (id != 0) scroll_into_view(id);
}

UiId UiTree::focused() const {
    return detail::ui_slot_of(slots_, focused_) == kUiNil ? 0 : focused_;
}

void UiTree::set_theme(AssetId<UiTheme> theme) {
    theme_ = theme;
    layout_dirty_ = true;
}

AssetId<UiTheme> UiTree::theme() const {
    return theme_;
}

void UiTree::clear() {
    // Slots (and their generations) are kept, so ids from before clear() never match new nodes.
    free_.clear();
    for (std::size_t i = slots_.size(); i-- > 0;) {
        Slot &s = slots_[i];
        if (s.alive) {
            s.alive = false;
            if (s.generation == 0xFFFFu) s.retired = true;
        }
        s.shown = false;
        s.block.reset();
        s.parent = s.first_child = s.last_child = s.next = s.prev = kUiNil;
        if (!s.retired) free_.push_back(static_cast<std::uint16_t>(i));
    }
    order_.clear();
    events_.clear();
    first_root_ = last_root_ = kUiNil;
    live_ = 0;
    focused_ = hovered_ = pressed_ = focus_root_ = 0;
    ++structure_version_;
    layout_dirty_ = true;
}

std::size_t UiTree::size() const {
    return live_;
}

UiId UiTree::first_child(UiId id) const {
    std::uint16_t c;
    if (id == 0) {
        c = first_root_;
    } else {
        const std::uint16_t index = detail::ui_slot_of(slots_, id);
        if (index == kUiNil) return 0;
        c = slots_[index].first_child;
    }
    return c == kUiNil ? 0 : detail::ui_id_of(slots_[c], c);
}

UiId UiTree::next_sibling(UiId id) const {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    if (index == kUiNil) return 0;
    const std::uint16_t n = slots_[index].next;
    return n == kUiNil ? 0 : detail::ui_id_of(slots_[n], n);
}

std::size_t UiTree::child_count(UiId id) const {
    std::size_t count = 0;
    for (UiId c = first_child(id); c != 0; c = next_sibling(c)) ++count;
    return count;
}

void UiTree::set_focus_root(UiId root) {
    if (root != 0 && detail::ui_slot_of(slots_, root) == kUiNil) return;
    focus_root_ = root;
}

UiId UiTree::focus_root() const {
    return detail::ui_slot_of(slots_, focus_root_) == kUiNil ? 0 : focus_root_;
}

std::span<const UiEvent> UiTree::events() const {
    return events_;
}

void UiTree::clear_events() {
    events_.clear();
}

bool UiTree::layout_dirty() const {
    return layout_dirty_;
}

Rect UiTree::screen_rect(UiId id) const {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    if (index == kUiNil || !slots_[index].shown) return {};
    return slots_[index].screen;
}

bool UiTree::shown(UiId id) const {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    return index != kUiNil && slots_[index].shown;
}

Rect UiTree::clip_rect(UiId id) const {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    if (index == kUiNil || !slots_[index].has_clip) return {};
    return slots_[index].clip;
}

Rect UiTree::value_rect(UiId id) const {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    if (index == kUiNil || !slots_[index].shown) return {};
    return slots_[index].value;
}

UiId UiTree::hovered() const {
    return detail::ui_slot_of(slots_, hovered_) == kUiNil ? 0 : hovered_;
}

UiId UiTree::pressed() const {
    return detail::ui_slot_of(slots_, pressed_) == kUiNil ? 0 : pressed_;
}

std::uint32_t UiTree::structure_version() const {
    return structure_version_;
}

UiTree::Stats UiTree::stats() const {
    return {measures_, wraps_, dropped_events_};
}

float UiTree::text_scale(UiId id) const {
    const std::uint16_t index = detail::ui_slot_of(slots_, id);
    return index == kUiNil ? 1.0f : slots_[index].scale;
}

} // namespace lam
