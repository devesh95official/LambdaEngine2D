#pragma once
// Engine menu scenes: menu, confirm, settings and text. Selections become intents and a tick
// system pops the scene with the matching SceneMessage, so menus replay exactly.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <lam/core/fixed_string.h>
#include <lam/core/id.h>
#include <lam/input/input_frame.h>
#include <lam/math/color.h>
#include <lam/scene/scene.h>
#include <lam/ui/ui.h>
namespace lam {
// One menu entry. `id` is the message kind a button closes with, or the settings key a value item
// edits; `value` is a button's payload or a value item's start value.
struct MenuItem {
    StrId id;
    FixedString<48> label;
    UiKind kind = UiKind::Button;                               // Button, Toggle, Slider, Choice, Label
    float value = 0, min = 0, max = 1;
    bool enabled = true;
    float step = 0;                                             // Slider; 0 = (max - min) / 10
    std::array<FixedString<24>, kMaxUiChoices> choices{};       // Choice options (clamped)
    std::uint8_t choice_count = 0;                              // 0: integer stepper min..max
};
// A whole menu: title, optional body text, the items, and how it looks and closes.
struct MenuSpec {
    FixedString<48> title;
    std::vector<MenuItem> items;
    bool overlay = true;                                        // lower scenes stay visible, dimmed
    Color dim{0, 0, 0, 140};
    bool back_pops = true;                                      // back -> pop(SceneMessage{"back"})
    std::string body;                                           // wrapped text under the title
    StrId back_kind = str_id("back");                           // SceneMessage kind sent by back
    StrId initial_focus;                                        // none: the first enabled item
    AssetId<UiTheme> theme;                                     // none: the built-in theme
    float width = 0;                                            // panel width; 0 = fit the items
};
// A Scene that shows `spec` in its world's UiTree and pops itself with the chosen item's
// SceneMessage at the next tick boundary. Subclass it to add hooks (see MenuState).
class MenuScene : public Scene {
public:
    explicit MenuScene(MenuSpec spec);
    std::string_view name() const override;                    // "lam.menu"
    // Builds the widgets, inserts MenuState and adds the menu's frame and tick systems.
    void enter(App &app, World &world) override;
    bool overlay() const override;                              // spec().overlay
    const MenuSpec &spec() const;
protected:
    MenuSpec spec_;
};
// "Yes"/"No" question. Focus starts on "No" (the safe answer); back answers no_kind. A question
// longer than the 47-byte title becomes the body text.
std::unique_ptr<Scene> make_confirm_scene(std::string_view question, StrId yes_kind, StrId no_kind = str_id("no"));
// Audio volumes, fullscreen, key rebinding and (LAM_DEV) show FPS, saved to SettingsStore, then
// the `extra` items and Back. Closes with SceneMessage{"back"}.
std::unique_ptr<Scene> make_settings_scene(std::span<const MenuItem> extra = {});
// Scrolling text page (credits, help) with a Close button. Closes with SceneMessage{"close"}.
std::unique_ptr<Scene> make_text_scene(std::string_view title, std::string body);

// ---- menu machinery: pure parts, testable without the scene stack ----
inline constexpr StrId kMenuSelectIntent = str_id("menu.select");   // a = item index, b = payload
inline constexpr StrId kMenuBackIntent = str_id("menu.back");
inline constexpr StrId kMenuRebindItem = str_id("input.rebind");    // settings: rebind buttons
struct MenuState;
// Frame-time hooks of special menus (settings, text). Plain function pointers.
using MenuChangeFn = void (*)(App &app, MenuState &state, std::size_t item, float value);
using MenuClickFn = bool (*)(App &app, MenuState &state, std::size_t item);   // true: handled
using MenuFrameFn = void (*)(App &app, MenuState &state, UiTree &tree, const FrameInput &input);
struct MenuState {                                              // World resource of a menu world
    MenuSpec spec;
    std::vector<UiId> nodes;                                    // nodes[i]: widget of spec.items[i]
    UiId root = 0, panel = 0, list = 0, body = 0;
    MenuChangeFn on_change = nullptr;                           // after a value widget changed
    MenuClickFn on_click = nullptr;                             // before a click becomes an intent
    MenuFrameFn on_frame = nullptr;                             // every frame after ui_update
    bool closed = false;                                        // closing: later intents are ignored
    std::optional<SceneMessage> result;                         // the message it closed with
    std::int64_t hook_state = 0;                                // free for the hooks (settings: capture)
    std::array<Intent, kMaxIntents> pending{};                  // fallback without InputSystem
    std::uint8_t pending_count = 0;
};
// Builds the menu widgets into `tree` and focuses the initial item. ui_update later shrinks the
// item list (then the body) if the panel doesn't fit on screen.
MenuState build_menu(UiTree &tree, MenuSpec spec, Vec2 screen);
// The index of the item whose widget is `node`; -1 if none.
int menu_item_index(const MenuState &state, UiId node);
// The intent a UiEvent turns into: click or submit -> "menu.select" (a = index, b = payload),
// back -> "menu.back". nullopt otherwise, or once the menu is closing.
std::optional<Intent> menu_event_intent(const MenuState &state, const UiTree &tree, const UiEvent &e);
// The message the menu closes with for `intent`. Reads only the intent and spec, so replays
// match. Marks the menu closed; nullopt for intents that don't close it.
std::optional<SceneMessage> menu_intent_message(MenuState &state, const Intent &intent);
// Default on_change: writes `value` to the SettingsStore key named by the item's id, if defined.
void menu_settings_change(App &app, MenuState &state, std::size_t item, float value);
}
