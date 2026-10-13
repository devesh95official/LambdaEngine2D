#pragma once
// Private UiTree storage shared by the lam_ui .cpp files. Children are an intrusive list in
// creation order, so document order is also draw order.
#include <cstdint>
#include <memory>
#include <string>

#include <lam/core/fixed_string.h>
#include <lam/render/font.h>
#include <lam/ui/ui.h>

namespace lam {

inline constexpr std::uint16_t kUiNil = 0xFFFFu;
inline constexpr std::size_t kUiMaxSlots = 0xFFFFu;             // slot indices 0..65534
inline constexpr std::size_t kUiMaxEvents = 64;

struct UiBlockData {                                            // TextBlock text and its cached wrap
    std::string text;
    TextLayout layout;
    std::uint32_t version = 1;                                  // ++ on every text change
    std::uint32_t laid_version = 0;
    float laid_width = -1.0f;
    float laid_size = -1.0f;
    const Font *laid_font = nullptr;
    TextAlign laid_align = TextAlign::Left;
    std::uint32_t laid_font_page = 0;
    int reveal = -1;                                            // glyphs drawn; -1 = all
};

struct UiTree::Slot {
    UiNode node;
    std::uint16_t generation = 0;                               // 0: never used
    bool alive = false;
    bool retired = false;                                       // generation exhausted: never reused
    std::uint16_t parent = kUiNil, first_child = kUiNil, last_child = kUiNil, next = kUiNil, prev = kUiNil;
    // ---- layout output ----
    Rect screen;                                                // absolute, virtual pixels
    Rect clip;                                                  // effective clip (when has_clip)
    Rect value;                                                 // slider bar / toggle box / choice area
    Vec2 natural;                                               // measured outer size
    bool shown = false;                                         // placed by the last layout
    bool has_clip = false;
    std::uint16_t max_scroll = 0;                               // List: children; TextBlock: lines
    std::uint16_t page_rows = 1;                                // List: rows that fit (wheel/page)
    float scale = 1.0f;                                         // text scale of the node's class
    float line_height = 8.0f;
    // ---- text measurement cache (labels re-measure only when this differs) ----
    FixedString<64> measured_text;
    const Font *measured_font = nullptr;
    float measured_scale = -1.0f;
    Vec2 text_size;
    std::unique_ptr<UiBlockData> block;                         // TextBlock only
};

struct UiTree::Frame {
    Rect screen;
    const UiTheme *theme = nullptr;                             // valid during layout() only
    const Font *font = nullptr;                                 // measuring font (never null in layout)
    bool real_font = false;
};

// Internal access for the .cpp files of lam_ui (the tree's private state).
struct UiTreeAccess {
    static std::vector<UiTree::Slot> &slots(UiTree &t) { return t.slots_; }
    static const std::vector<UiTree::Slot> &slots(const UiTree &t) { return t.slots_; }
    static const std::vector<std::uint16_t> &order(const UiTree &t) { return t.order_; }
    static UiId focused(const UiTree &t) { return t.focused_; }
    static UiId pressed(const UiTree &t) { return t.pressed_; }
    static UiId hovered(const UiTree &t) { return t.hovered_; }
};

namespace detail {
// Slot index of a live id, or kUiNil.
std::uint16_t ui_slot_of(const std::vector<UiTree::Slot> &slots, UiId id);
UiId ui_id_of(const UiTree::Slot &s, std::uint16_t index);
// The 6x8 monospace metrics font used when no real font is available (measuring only).
const Font &ui_fallback_font();
// Whether the slot and all its ancestors are visible.
bool ui_visible_chain(const std::vector<UiTree::Slot> &slots, std::uint16_t index);
// "< value >" / "On" / number: the value text of a value widget (no allocation).
std::string_view ui_value_text(const UiNode &n, FixedString<32> &scratch);
// The theme class of a node (base theme when none/unknown), resolved into `out`.
const UiThemeClass &ui_class_of(const UiTheme &theme, const UiNode &n, UiThemeClass &scratch);
float ui_text_scale(const Font &f, float size);
// Layout constants shared by layout, input and drawing.
inline constexpr float kUiValueWidth = 64.0f;                   // slider bar / choice area
inline constexpr float kUiValueGap = 8.0f;                      // between label and value area
inline constexpr float kUiArrowWidth = 12.0f;                   // "<" and ">" areas of a choice
inline constexpr float kUiInputMinWidth = 80.0f;                // text input
}

} // namespace lam
