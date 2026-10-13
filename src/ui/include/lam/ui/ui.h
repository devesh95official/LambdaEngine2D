#pragma once
// Retained UI: one UiTree per world, drawn through RenderQueue at screen layers. UiEvent is
// presentation only; to change the game, queue an intent.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <lam/core/asset_id.h>
#include <lam/core/fixed_string.h>
#include <lam/core/id.h>
#include <lam/core/result.h>
#include <lam/input/input_frame.h>
#include <lam/math/color.h>
#include <lam/math/rect.h>
#include <lam/render/font.h>
#include <lam/render/texture.h>
namespace lam {
// Slot index in the low 16 bits, generation in the high 16, so stale ids read as absent.
using UiId = std::uint32_t;                                     // 0 = none
// Widget kinds. Button, Toggle, Slider, Choice and TextInput are interactive (always focusable).
enum class UiKind : std::uint8_t { Panel, Label, Button, Toggle, Slider, Choice, Image, List, TextInput, TextBlock };
// How a container places its visible children: None = by anchor/pivot/rect; Vertical and
// Horizontal stack them (anchor aligns across the stack); Grid fills style.grid_columns columns.
enum class UiLayout : std::uint8_t { None, Vertical, Horizontal, Grid };
// Per-node style: theme class (UiThemeClass::id; none = the base theme), inner padding (pixels),
// spacing between stacked children (pixels) and the column count of Grid layouts.
struct UiStyle { StrId theme_class; Vec2 padding{4, 4}; float spacing = 2; std::uint8_t grid_columns = 1; };
// Choices per Choice widget (the index is always clamped into [0, choice_count)).
inline constexpr int kMaxUiChoices = 8;
// UiNode::fill bits: take the parent's content width/height (None layouts and stacks' cross axis).
enum UiFill : std::uint8_t { kUiFillX = 1, kUiFillY = 2 };
// One widget. Read and edit it through UiTree::get; `parent` is set by UiTree::add and must not
// be changed afterwards (the tree keeps its own child links).
struct UiNode {
    UiKind kind = UiKind::Panel;
    UiId parent = 0;
    StrId id;                                                   // reported in UiEvent; UiTree::find
    Rect rect;                                                  // relative; w/h 0 = natural size
    Vec2 anchor{0, 0}, pivot{0, 0};                             // 0..1 of the parent / own size
    UiLayout layout = UiLayout::None;
    UiStyle style;
    FixedString<64> text;                                       // label of every text widget
    bool enabled = true, visible = true, focusable = false;     // invisible: no space, no input
    float value = 0, min = 0, max = 1, step = 0.1f;             // slider / toggle / choice index
    std::array<FixedString<24>, kMaxUiChoices> choices{};
    std::uint8_t choice_count = 0;                              // 0 on a Choice: integer stepper min..max
    TextureRegion image;
    std::uint16_t scroll = 0;                                   // List: first child; TextBlock: line
    std::uint8_t fill = 0;                                      // UiFill bits
    Vec2 max_size{0, 0};                                        // caps the natural size; 0 = no cap
    Color color = kTransparent;                                 // alpha != 0 overrides the theme
    StrId intent;                                               // queued as Intent{intent, intent_a, value}
    std::int64_t intent_a = 0;
};
// A user action: Clicked (button, focusable image or panel), Changed (value widgets; `value` is
// the new value), Focused, Back (node = the focus or 0) or Submitted (text field).
struct UiEvent {                                                // World event (Update -800)
    enum class Kind : std::uint8_t { Clicked, Changed, Focused, Back, Submitted };
    Kind kind = Kind::Clicked;
    UiId node = 0;                                              // the widget (0 for Back without focus)
    StrId id;                                                   // its UiNode::id
    float value = 0;                                            // its UiNode::value after the action
};
// A theme class (".theme.json" "classes"): every field starts as the base theme's value.
struct UiThemeClass {
    StrId id;                                                   // UiStyle::theme_class that selects it
    float font_size = 0;                                        // 0 = the theme's
    Color text{240, 240, 240, 255}, text_disabled{120, 120, 120, 255};   // as in UiTheme
    Color panel{24, 24, 32, 230}, panel_border{90, 90, 110, 255};
    Color button{48, 48, 64, 255}, button_focus{80, 80, 120, 255}, button_down{30, 30, 40, 255};
    bool background = true;                                     // panels and lists draw a fill/frame
    TextAlign align = TextAlign::Left;                          // labels; buttons centre unless set
    bool align_set = false;                                     // "align" given in the JSON
};
// Colours, font and nine-slice of a tree (UiTree::set_theme). Widgets tint the nine-slice with
// panel / button / button_focus / button_down; without one they draw flat fills and 1 px frames.
struct UiTheme {                                                // asset ".theme.json"; built-in default if none
    AssetId<Font> font;                                         // none: RenderDefaults::font
    float font_size = 0;                                        // pixels; 0 = the font's base size
    // Text, panel fill/frame and button fills (idle, focused, pressed); classes override them.
    Color text{240, 240, 240, 255}, text_disabled{120, 120, 120, 255};
    Color panel{24, 24, 32, 230}, panel_border{90, 90, 110, 255};
    Color button{48, 48, 64, 255}, button_focus{80, 80, 120, 255}, button_down{30, 30, 40, 255};
    Color overlay_dim{0, 0, 0, 140};                            // behind modal popups
    TextureRegion nine_slice;                                   // none -> flat colours
    float nine_slice_border = 4;
    StrId click_sound, focus_sound;                             // PlaySound groups (optional)
    std::vector<UiThemeClass> classes;                          // sorted by id value; load time only
    StrId atlas_group = str_id("default");                      // none = "atlas": "none"
    // The class `id`; null for none or unknown ids (callers fall back to the base theme).
    const UiThemeClass *find_class(StrId id) const;
    // The base theme as a class (id none): what every class starts from.
    UiThemeClass base_class() const;
};
// The built-in theme (no font: RenderDefaults::font is used). Static storage.
const UiTheme &default_ui_theme();
class LoadContext;
template <class T> class AssetLoader;                           // <lam/assets/asset_server.h>
// Parses a .theme.json document (strict). Font and nine-slice paths become dependencies through
// `deps`; with null they're checked but left unset.
Result<UiTheme> parse_ui_theme(std::string_view json, std::string_view file_name, LoadContext *deps = nullptr);
// The ".theme.json" loader (UiPlugin registers it). Finalize packs the nine-slice image into the
// theme's atlas group when its pixels are present, else keeps the standalone texture.
std::unique_ptr<AssetLoader<UiTheme>> make_ui_theme_loader();
// One frame of input for a UiTree, independent of lam_input so navigation is testable.
// ui_update fills it from FrameInput + UiNav (directions use is_repeated, so held keys repeat).
struct UiInput {
    bool up = false, down = false, left = false, right = false;   // pressed or repeated this frame
    bool confirm = false, back = false;                            // pressed this frame
    bool erase = false;                                            // TextInput backspace (repeats)
    Vec2 pointer;                                                  // virtual pixels
    bool pointer_valid = false;
    bool pointer_down = false, pointer_pressed = false, pointer_released = false;  // left / touch 0
    float wheel = 0;                                               // > 0 scrolls lists up
    std::span<const char32_t> typed;                               // TextInput text this frame
};
class UiTree {                                                  // World resource (init_world)
public:
    UiTree();
    ~UiTree();
    UiTree(const UiTree &) = delete;
    UiTree &operator=(const UiTree &) = delete;
    // Adds `n` as the last child of `parent` (0 = a root). Returns 0 if `parent` is dead.
    // Interactive kinds become focusable.
    UiId add(UiId parent, const UiNode &n);
    // Builders: add() with the kind's defaults. Text is truncated to fit FixedString<64>.
    UiId panel(UiId parent, Rect r, UiLayout l = UiLayout::Vertical);   // r.w/h 0: fit children
    UiId label(UiId parent, std::string_view text);             // not focusable; skipped by navigation
    UiId button(UiId parent, StrId id, std::string_view label); // Clicked on confirm / click
    UiId toggle(UiId parent, StrId id, std::string_view label, bool on);   // value 0/1; Changed
    // Value in [mn, mx] snapped to `step` from mn (0 = (mx - mn) / 10); left/right step, drag sets.
    UiId slider(UiId parent, StrId id, std::string_view label, float v, float mn, float mx, float step = 0.1f);
    // Value = the selected index (clamped); left/right and clicks on the arrows cycle, wrapping.
    UiId choice(UiId parent, StrId id, std::string_view label, std::span<const std::string_view> choices, int selected);
    UiId list(UiId parent, Rect r);                             // vertical scrolling list (clips)
    UiId text_block(UiId parent, Rect r, std::string text);     // long text; laid out once (allocates)
    // A scrolling grid: a List with Grid layout and `columns` columns (inventories, level select).
    UiId grid(UiId parent, Rect r, int columns);
    // An image of `region` (size 0 = the region's size); focusable when `id` is not none.
    UiId image(UiId parent, const TextureRegion &region, Vec2 size = {}, StrId id = {});
    // Single-line text field. Confirm emits Submitted and moves focus down; while it's focused the
    // direction keys type instead of navigating.
    UiId text_input(UiId parent, StrId id, std::string_view initial = {});
    void remove(UiId id);                                       // and children
    UiNode *get(UiId id);                                       // valid until the next add/remove
    const UiNode *get(UiId id) const;                           // null for dead or stale ids
    UiId find(StrId id) const;                                  // first live node with that id; 0 if none
    void set_text(UiId id, std::string_view text);              // no allocation; marks layout dirty
    void set_value(UiId id, float value);                       // sanitised like add(); no event
    void focus(UiId id);                                        // programmatic: no Focused event; 0 clears
    UiId focused() const;                                       // 0 if none (or it died)
    void set_theme(AssetId<UiTheme> theme);                     // none: default_ui_theme()
    void clear();                                               // removes every node; old ids stay dead
    std::size_t size() const;                                   // live nodes

    bool alive(UiId id) const;                                  // id names a live node
    AssetId<UiTheme> theme() const;                             // as given to set_theme
    // TextBlock text (allocates when it grows); set_text on a TextBlock does the same.
    void set_block_text(UiId id, std::string text);
    std::string_view block_text(UiId id) const;                 // "" for other kinds
    // Typewriter reveal of a TextBlock: at most `glyphs` glyphs are drawn; -1 = all.
    void set_reveal(UiId id, int glyphs);
    // Replaces a Choice's options (truncated to kMaxUiChoices and 23 bytes each).
    void set_choices(UiId id, std::span<const std::string_view> choices, int selected);
    // Children in order (0 when there are none / at the end).
    UiId first_child(UiId id) const;                            // id 0: the first root
    UiId next_sibling(UiId id) const;
    std::size_t child_count(UiId id) const;
    // Confines navigation and pointer input to this subtree (0 = whole tree), for modal popups.
    void set_focus_root(UiId root);
    UiId focus_root() const;
    // Scrolls a List by `rows` (grid rows), or a TextBlock by lines; clamped.
    void scroll_by(UiId id, int rows);
    // Scrolls every List ancestor so `id` is visible, deferred to the next layout if needed.
    void scroll_into_view(UiId id);

    // Lays out every node inside `screen` (virtual pixels). Labels re-measure and TextBlocks
    // re-wrap only when something they depend on changed. Null font: 6x8 fallback metrics.
    void layout(Rect screen, const UiTheme &theme, const Font *font);
    // One frame of input, after layout(). Each action produces at most one event.
    void handle_input(const UiInput &in);
    std::span<const UiEvent> events() const;                    // since clear_events(); max 64
    void clear_events();
    bool layout_dirty() const;                                  // input changed text, values or scroll
    // After layout(): screen rect, whether it was placed (not hidden or scrolled out), and clip.
    Rect screen_rect(UiId id) const;
    bool shown(UiId id) const;
    Rect clip_rect(UiId id) const;                              // {} = unclipped
    // Slider bar, toggle box or choice value area inside the widget ({} for other kinds).
    Rect value_rect(UiId id) const;
    // Topmost shown node under `p` (any kind; respects clips and the focus root); 0 if none.
    UiId hit_test(Vec2 p) const;
    UiId hovered() const;                                       // focusable node under the pointer
    UiId pressed() const;                                       // pointer pressed on it, not released
    std::uint32_t structure_version() const;                    // ++ on every add/remove/clear
    // Text scale (theme class size / font base size) of node `id` in the last layout.
    float text_scale(UiId id) const;
    // Diagnostics: label measures and TextBlock wraps by layout(), and events over the cap.
    struct Stats { std::uint32_t measures = 0, wraps = 0, dropped_events = 0; };
    Stats stats() const;

    struct Slot;                                                // private storage (ui_tree.cpp)
    struct Frame;
private:
    std::vector<Slot> slots_;
    std::vector<std::uint16_t> free_;
    std::vector<std::uint16_t> order_;                          // document order (slot indices)
    std::vector<UiEvent> events_;
    std::unique_ptr<Frame> frame_;                              // layout context of the last layout()
    std::uint16_t first_root_ = 0xFFFFu, last_root_ = 0xFFFFu;
    std::uint32_t structure_version_ = 1;
    std::uint32_t order_version_ = 0;
    std::size_t live_ = 0;
    UiId focused_ = 0, hovered_ = 0, pressed_ = 0, focus_root_ = 0;
    UiId pending_scroll_ = 0;                                   // scroll_into_view before its list was laid out
    AssetId<UiTheme> theme_;
    Vec2 last_pointer_{-1e9f, -1e9f};
    bool layout_dirty_ = true;
    std::uint32_t dropped_events_ = 0;
    std::uint32_t measures_ = 0, wraps_ = 0;
    friend struct UiTreeAccess;
    friend struct UiInputOps;
};
// Default actions "ui.up" .. "ui.erase": arrows/WASD + d-pad, Enter/Space + South, Escape + East,
// Backspace.
struct UiNav { ButtonAction up, down, left, right, confirm, back; ButtonAction erase; };   // App resource
// UiInput from frame input. `typed` stays empty; ui_update fills it from InputSystem.
UiInput ui_input_from(const FrameInput &frame, const UiNav &nav);
}
