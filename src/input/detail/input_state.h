#pragma once
// Private state of ActionMap and InputSystem. Fixed-size so frame and tick code never allocates.
#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/input/raw_input.h>
#include <lam/math/vec2.h>

namespace lam {
struct CameraView;
struct VirtualLayout;

// Slots of one button action for one player. `count` = 1 + the last non-empty slot.
struct ButtonSlots {
    std::array<Binding, kMaxBindingSlots> slot{};
    std::uint8_t count = 0;
};
// Composite and stick slots of one axis action for one player.
struct AxisSlots {
    std::array<Axis2Binding, kMaxBindingSlots> keys{};
    std::uint8_t key_count = 0;
    std::array<StickBinding, kMaxBindingSlots> sticks{};
    std::uint8_t stick_count = 0;
};

enum class CaptureKind : std::uint8_t { None, Button, Axis };

struct ActionMap::Impl {
    // index = id, [0] unused. Deque so name_of() views survive new definitions (SSO strings
    // would move in a vector).
    std::deque<std::string> button_names{std::string()};
    std::deque<std::string> axis_names{std::string()};
    std::array<bool, kMaxAxisActions> axis_2d{};
    std::array<std::array<ButtonSlots, kMaxPlayers>, kMaxButtonActions> buttons{};
    std::array<ButtonSlots, kMaxButtonActions> button_defaults{};   // player 0
    std::array<std::array<AxisSlots, kMaxPlayers>, kMaxAxisActions> axes{};
    std::array<AxisSlots, kMaxAxisActions> axis_defaults{};         // player 0
    std::array<std::uint16_t, kMaxButtonActions> repeat_delay{};
    std::array<std::uint16_t, kMaxButtonActions> repeat_interval{};
    std::array<std::int8_t, kMaxPlayers> pad{kAnyPad, kNoPad, kNoPad, kNoPad};
    std::array<bool, kMaxPlayers> keyboard_mouse{true, true, true, true};
    std::array<bool, kMaxPlayers> assigned{};
    std::array<TouchZone, kMaxTouchZones> zones{};
    int zone_count = 0;
    std::uint32_t zone_layout = 0;                              // ++ on clear_touch_zones: captures end
    CaptureKind capture = CaptureKind::None;
    std::uint16_t capture_id = 0;
    int capture_player = 0;
    int capture_slot = 0;
    AxisDir capture_dir = AxisDir::Up;
    std::uint32_t revision = 0;
    // (action, player) pairs whose settings keys were applied (load or sync_settings).
    std::array<std::array<bool, kMaxPlayers>, kMaxButtonActions> button_synced{};
    std::array<std::array<bool, kMaxPlayers>, kMaxAxisActions> axis_synced{};
    // Bindings as they were when define_settings first saw the pair. A stored value equal to
    // these isn't treated as a user override.
    std::array<std::array<std::array<Binding, kMaxBindingSlots>, kMaxPlayers>, kMaxButtonActions> button_def{};
    std::array<std::array<bool, kMaxPlayers>, kMaxButtonActions> button_def_taken{};
    std::array<std::array<std::array<Axis2Binding, kMaxBindingSlots>, kMaxPlayers>, kMaxAxisActions> axis_def{};
    std::array<std::array<bool, kMaxPlayers>, kMaxAxisActions> axis_def_taken{};
};

// Per-player live state.
struct PlayerLive {
    ActionBits down{};                                          // this frame
    ActionBits prev_down{};                                     // previous frame
    ActionBits wheel_only{};                                    // down only through wheel bindings
    ActionBits latch_pressed{};                                 // not yet delivered to a tick
    ActionBits latch_released{};
    std::array<Vec2, kMaxAxisActions> axes{};                   // this frame
    std::array<std::uint32_t, kMaxButtonActions> held_ticks{};  // tick repeat counters
};

// A touch zone's capture.
struct ZoneLive {
    int touch_id = -1;                                          // -1 = free
    Vec2 origin;                                                // virtual px where the touch began
    Vec2 pos;                                                   // current virtual px
    bool held = false;
};

struct InputSystem::Impl {
    RawInput raw{};                                             // as fed (deltas zeroed when stale)
    RawInput prev_raw{};                                        // previous frame (capture edges)
    bool fed = false;                                           // feed_raw since the last frame update
    std::array<char32_t, kMaxTextPerFrame> text{};
    int text_count = 0;
    std::array<PlayerLive, kMaxPlayers> players{};
    // Pointer (mouse, or touch 0 when touches exist).
    Vec2 pointer_virtual;
    bool pointer_valid = false;
    std::uint8_t pointer_down = 0;
    std::uint8_t prev_pointer_down = 0;
    std::uint8_t latch_pointer_pressed = 0;
    float latch_wheel = 0;
    // FrameInput real-time repeat (player 0), seconds.
    std::array<float, kMaxButtonActions> held_seconds{};
    std::array<float, kMaxButtonActions> next_repeat{};
    std::array<ZoneLive, kMaxTouchZones> zones{};
    std::uint32_t seen_zone_layout = 0;
    std::array<int, kMaxTouches> prev_touch_ids{};
    int prev_touch_count = 0;
    std::array<std::array<Intent, kMaxIntents>, kMaxPlayers> intents{};
    std::array<std::uint8_t, kMaxPlayers> intent_count{};
    std::uint32_t dropped = 0;
    InputDriver *driver = nullptr;
    InputRecorder *recorder = nullptr;
    bool last_paused = false;
    std::uint32_t focus_id = 0;                                 // focused World::id(), 0 = none
    std::uint32_t mirrored_revision = 0;                        // last revision written to settings
    bool mirrored_once = false;
    std::string record_path;                                    // --record-input, opened at the first tick
    bool record_pending = false;
};

// What a frame update needs from the App. Tests can fill it by hand.
struct FrameEnv {
    const VirtualLayout *layout = nullptr;                      // null: virtual = framebuffer
    const CameraView *view = nullptr;                           // focused world's; null/invalid: world = virtual
    float frame_dt = 0;                                         // seconds
    float tick_dt = 1.0f / 60;                                  // seconds per tick (repeat timing)
    bool paused = false;
    std::uint32_t focus_id = 0;
    bool lost_focus = false;                                    // FocusLost/Background this frame
};

namespace detail {
struct InputAccess {
    using MapImpl = ActionMap::Impl;
    using SysImpl = InputSystem::Impl;
    static ActionMap::Impl &map(ActionMap &m) { return *m.impl_; }
    static const ActionMap::Impl &map(const ActionMap &m) { return *m.impl_; }
    static InputSystem::Impl &sys(InputSystem &s) { return *s.impl_; }
    static const InputSystem::Impl &sys(const InputSystem &s) { return *s.impl_; }
};

void input_frame_update(InputSystem &sys, ActionMap &map, FrameInput &out, const FrameEnv &env);
// Moves the latch and pending intents into ActionState.
void input_publish(InputSystem &sys, const ActionMap &map, ActionState &state, const CameraView *view,
                   std::uint32_t focus_id);
// Identity when the view is null or invalid.
Vec2 to_world(const CameraView *view, Vec2 virtual_pos);
// count = 1 + the last non-empty slot.
void recount(ButtonSlots &s);
void recount(AxisSlots &s);
Binding &dir_ref(Axis2Binding &b, AxisDir dir);
// Runs when the map's revision changes. Applies stored values to actions defined after the last
// load (without undoing in-memory rebinds), then writes every binding back.
void sync_settings(ActionMap &map, SettingsStore &s);
// Copy with zeroed padding so identical input gives identical replay files.
void canonical_frame_bytes(const InputFrame &f, std::byte *out);
} // namespace detail
} // namespace lam
