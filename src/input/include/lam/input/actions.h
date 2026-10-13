#pragma once
// Named actions and per-player bindings, saved to SettingsStore as "input.p<player>.<action>.<slot>"
// (format in src/input/README.md). Define actions at startup; reading them never allocates.
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <lam/core/result.h>
#include <lam/core/settings.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/math/rect.h>
namespace lam {
namespace detail {
struct InputAccess;
}
// Decides how Binding::code is read.
enum class BindingSource : std::uint8_t { None, Key, Mouse, PadButton, PadAxisPositive, PadAxisNegative,
    WheelUp, WheelDown, TouchZone };
// One physical input. Pad axes count as down past `threshold`; in axis composites they give
// an analog value rescaled from threshold..1, everything else gives 0 or 1.
struct Binding {
    BindingSource source = BindingSource::None;
    std::uint16_t code = 0;                                     // Key / MouseButton / PadButton / PadAxis value
    std::int8_t pad = -1;                                       // -1 = any pad assigned to the player
    float threshold = 0.5f;                                     // axes
    bool operator==(const Binding &) const = default;
};
// Four bindings (WASD), +y down, diagonals normalised. axis1 only uses left and right.
struct Axis2Binding { Binding up, down, left, right; bool operator==(const Axis2Binding &) const = default; };
// Two pad axes with a radial deadzone; the rest of the range is rescaled to 0..1.
struct StickBinding { PadAxis x = PadAxis::LeftX, y = PadAxis::LeftY; float deadzone = 0.2f; std::int8_t pad = -1;
    bool operator==(const StickBinding &) const = default; };
// On-screen control for player 0. A touch that starts in the rect holds `button` and drives
// `stick` as a floating stick until it lifts. Either may be empty.
struct TouchZone { Rect virtual_rect; ButtonAction button; AxisAction stick; };
inline constexpr int kMaxBindingSlots = 4;                      // slots per action and player
inline constexpr int kAnyPad = -1;                              // assign_player: every connected pad
inline constexpr int kNoPad = -2;                               // assign_player: no pad
inline constexpr int kMaxTouchZones = 16;                       // add_touch_zone capacity
// For axis1, Left is negative and Right positive.
enum class AxisDir : std::uint8_t { Up, Down, Left, Right };
class ActionMap {                                               // App resource
public:
    // Player 0 gets keyboard/mouse and every pad, players 1..3 keyboard/mouse only.
    ActionMap();
    ~ActionMap();
    ActionMap(const ActionMap &other);
    ActionMap &operator=(const ActionMap &other);
    ActionMap(ActionMap &&) noexcept;
    ActionMap &operator=(ActionMap &&) noexcept;
    // Defines the action, or returns the existing id. Defaults bind player 0 and only the first
    // call sets them. Asserts past 255 actions or if the name is taken by an axis.
    ButtonAction button(std::string_view name, std::initializer_list<Binding> defaults = {});
    // Same rules as button(). Composites and sticks are summed and clamped to length 1.
    AxisAction axis2(std::string_view name, std::initializer_list<Axis2Binding> keys = {},
                     std::initializer_list<StickBinding> sticks = {});
    // x = positive - negative in [-1, 1]. Slot i pairs negative[i] with positive[i].
    AxisAction axis1(std::string_view name, std::initializer_list<Binding> positive,
                     std::initializer_list<Binding> negative);
    ButtonAction find_button(std::string_view name) const;      // {} if unknown
    AxisAction find_axis(std::string_view name) const;          // {} if unknown
    std::string_view name_of(ButtonAction a) const;             // "" if unknown
    std::string_view name_of(AxisAction a) const;               // "" if unknown
    int button_count() const;                                   // ids are 1..count
    int axis_count() const;
    bool is_axis2(AxisAction a) const;
    // is_repeated() fires on the press, after delay_ticks, then every interval_ticks.
    // interval 0 turns repeat off.
    void set_repeat(ButtonAction a, std::uint16_t delay_ticks, std::uint16_t interval_ticks);
    std::uint16_t repeat_delay(ButtonAction a) const;           // ticks; 0 if unset
    std::uint16_t repeat_interval(ButtonAction a) const;        // ticks; 0 = no repeat
    // `pad` is 0..3, kAnyPad or kNoPad. Bindings with their own Binding::pad ignore this.
    void assign_player(int player, int pad, bool keyboard_mouse);
    int player_pad(int player) const;                           // kAnyPad, kNoPad or 0..3
    bool player_keyboard_mouse(int player) const;
    // 1 + the highest player with a binding or an assignment.
    int player_count() const;
    // A None binding clears the slot.
    void rebind(ButtonAction a, int player, int slot, Binding b);
    void clear_bindings(ButtonAction a, int player);
    // Up to the last non-empty slot. Valid until this action's bindings change.
    std::span<const Binding> bindings(ButtonAction a, int player = 0) const;
    void rebind(AxisAction a, int player, int slot, const Axis2Binding &b);
    void rebind(AxisAction a, int player, int slot, AxisDir dir, Binding b);
    void bind_stick(AxisAction a, int player, int slot, StickBinding s);
    void clear_bindings(AxisAction a, int player);
    std::span<const Axis2Binding> axis_bindings(AxisAction a, int player = 0) const;
    std::span<const StickBinding> stick_bindings(AxisAction a, int player = 0) const;
    // Player 0 goes back to the defaults, other players are cleared.
    void reset_bindings(int player);
    // The next new press becomes the binding; Escape cancels. Nothing else reports presses
    // while capturing, so the captured key doesn't also trigger an action.
    void begin_capture(ButtonAction a, int player, int slot);
    void begin_capture(AxisAction a, int player, int slot, AxisDir dir);
    bool capturing() const;
    void cancel_capture();
    // Zone index = call order, which is what TouchZone binding codes refer to.
    void add_touch_zone(const TouchZone &z);
    std::span<const TouchZone> touch_zones() const;
    void clear_touch_zones();
    // Bindings already in memory become the key defaults, so a stored value equal to them never
    // counts as an override. Called by InputPlugin::finish before load().
    void define_settings(SettingsStore &s) const;
    Status save(SettingsStore &s) const;
    // "none" clears a slot. Bad entries are logged and leave the binding alone.
    Status load(const SettingsStore &s);
    // Bumped on any binding, assignment, repeat or zone change.
    std::uint32_t revision() const;
    static Binding key(Key k);
    static Binding mouse(MouseButton b);                        // touch 0 counts as Left
    static Binding pad(PadButton b, int pad = -1);              // -1 = the player's pads
    static Binding pad_axis(PadAxis a, bool positive, float threshold = 0.5f, int pad = -1);
    // Each frame with wheel motion is a fresh press, so one notch reaches one tick.
    static Binding wheel(bool up);
    static Binding touch(int zone);
private:
    friend struct detail::InputAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// "none", "key:space", "key:#123", "mouse:left", "pad:south@1", "axis:-rt@2>0.25", "wheel:up",
// "touch:0". The threshold is only written when it isn't 0.5.
std::string binding_to_string(const Binding &b);
// Case-insensitive; nullopt if malformed.
std::optional<Binding> binding_from_string(std::string_view s);
}
