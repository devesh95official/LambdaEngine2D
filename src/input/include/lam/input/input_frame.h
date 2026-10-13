#pragma once
// InputFrame is latched per tick and recorded for replays. FrameInput is the unlatched
// per-frame view for UI, which keeps working while ticks are paused.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <lam/core/app.h>
#include <lam/core/id.h>
#include <lam/math/vec2.h>
namespace lam {
inline constexpr int kMaxButtonActions = 256;
inline constexpr int kMaxAxisActions = 16;
inline constexpr int kMaxPlayers = 4;
inline constexpr int kMaxIntents = 4;
struct ButtonAction {
    std::uint16_t id = 0;
    constexpr explicit operator bool() const { return id != 0; }
    constexpr bool operator==(const ButtonAction &) const = default;
};
struct AxisAction {
    std::uint8_t id = 0;
    constexpr explicit operator bool() const { return id != 0; }
    constexpr bool operator==(const AxisAction &) const = default;
};
// A UI decision handed to the simulation in exactly one tick. a/b are free payload. If a rule
// can't act on it that tick, it has to copy it into saved state or it's lost.
struct Intent { StrId kind; std::int64_t a = 0, b = 0; };
using ActionBits = std::array<std::uint64_t, 4>;
constexpr bool action_bit(const ActionBits &bits, ButtonAction a) {
    return a.id != 0 && a.id < kMaxButtonActions && ((bits[a.id >> 6] >> (a.id & 63u)) & 1u) != 0;
}
constexpr void set_action_bit(ActionBits &bits, ButtonAction a, bool on = true) {
    if (a.id == 0 || a.id >= kMaxButtonActions) return;
    const std::uint64_t bit = std::uint64_t{1} << (a.id & 63u);
    bits[a.id >> 6] = on ? (bits[a.id >> 6] | bit) : (bits[a.id >> 6] & ~bit);
}
struct InputFrame {
    ActionBits pressed{};                                       // each press shows up in one tick
    ActionBits held{};
    ActionBits released{};
    ActionBits repeated{};
    std::array<Vec2, kMaxAxisActions> axes{};                   // 1D axes use .x
    Vec2 pointer_virtual;                                       // virtual pixels
    Vec2 pointer_world;
    bool pointer_valid = false;
    std::uint8_t pointer_down = 0;                              // MouseButton bits (touch 0 = Left)
    std::uint8_t pointer_pressed = 0;
    float wheel = 0;
    std::array<Intent, kMaxIntents> intents{};
    std::uint8_t intent_count = 0;
    bool is_pressed(ButtonAction a) const { return action_bit(pressed, a); }
    bool is_held(ButtonAction a) const { return action_bit(held, a); }
    bool is_released(ButtonAction a) const { return action_bit(released, a); }
    bool is_repeated(ButtonAction a) const { return action_bit(repeated, a); }
    Vec2 axis2(AxisAction a) const { return a.id < kMaxAxisActions ? axes[a.id] : Vec2{}; }
    float axis1(AxisAction a) const { return axis2(a).x; }
    std::span<const Intent> intent_list() const { return std::span<const Intent>(intents.data(), intent_count); }
    // Writers for InputDriver::fill. The frame already holds this tick's intents, so use
    // clear_devices() rather than `frame = InputFrame{}` to replace the player.
    // Use press() on the first tick and hold() on the ticks after it.
    void press(ButtonAction a) {
        set_action_bit(pressed, a);
        set_action_bit(held, a);
        set_action_bit(repeated, a);
    }
    void hold(ButtonAction a) { set_action_bit(held, a); }
    void release(ButtonAction a) {
        set_action_bit(held, a, false);
        set_action_bit(released, a);
    }
    // |v| <= 1, +y down; 1D axes use v.x.
    void set_axis(AxisAction a, Vec2 v) {
        if (a.id != 0 && a.id < kMaxAxisActions) axes[a.id] = v;
    }
    // False when kMaxIntents are already queued.
    bool push_intent(const Intent &i) {
        if (intent_count >= kMaxIntents) return false;
        intents[intent_count++] = i;
        return true;
    }
    // Keeps the intents.
    void clear_devices() {
        pressed = held = released = repeated = ActionBits{};
        axes = {};
        pointer_virtual = pointer_world = Vec2{};
        pointer_valid = false;
        pointer_down = pointer_pressed = 0;
        wheel = 0;
    }
};
static_assert(std::is_trivially_copyable_v<InputFrame>);
struct ActionState {                                            // App resource, written at TickBegin
    std::array<InputFrame, kMaxPlayers> players{};
};
struct PointerState {
    Vec2 virtual_pos;
    Vec2 world;                                                 // via last frame's CameraView
    bool valid = false;
    std::uint8_t down = 0, pressed = 0, released = 0;           // MouseButton bits, this frame
    float wheel = 0;
};
struct FrameInput {                                             // App resource; player 0 only
    ActionBits pressed{};
    ActionBits held{};
    ActionBits released{};
    ActionBits repeated{};                                      // repeat timed in real seconds
    std::array<Vec2, kMaxAxisActions> axes{};
    PointerState pointer;
    bool is_pressed(ButtonAction a) const { return action_bit(pressed, a); }
    bool is_held(ButtonAction a) const { return action_bit(held, a); }
    bool is_released(ButtonAction a) const { return action_bit(released, a); }
    bool is_repeated(ButtonAction a) const { return action_bit(repeated, a); }
    Vec2 axis2(AxisAction a) const { return a.id < kMaxAxisActions ? axes[a.id] : Vec2{}; }
};
// Neutral if this world isn't focused.
inline const InputFrame &input_frame(const Ctx &ctx, int player = 0) {
    static const InputFrame neutral{};
    const ActionState *s = ctx.try_res<ActionState>();
    if (s == nullptr || !ctx.focused() || player < 0 || player >= kMaxPlayers) return neutral;
    return s->players[static_cast<std::size_t>(player)];
}
// Neutral if this world isn't focused.
inline const FrameInput &frame_input(const Ctx &ctx) {
    static const FrameInput neutral{};
    const FrameInput *f = ctx.try_res<FrameInput>();
    return (f == nullptr || !ctx.focused()) ? neutral : *f;
}
}
