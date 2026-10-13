#pragma once
// One frame of raw device state, fed by the backend or by tests. Wheel, text and touch `began`
// are deltas and only count once; the rest holds until the next feed.
#include <array>
#include <cstdint>
#include <lam/input/keys.h>
#include <lam/math/vec2.h>
namespace lam {
inline constexpr int kMaxGamepads = 4;
inline constexpr int kMaxTouches = 8;
inline constexpr int kMaxTextPerFrame = 16;
struct RawTouch { int id = -1; Vec2 pos_fb; bool began = false; };
struct RawInput {
    std::array<std::uint64_t, 6> keys_down{};                   // bit = Key value
    std::uint8_t mouse_down = 0;                                // bit = MouseButton
    Vec2 mouse_fb;                                              // framebuffer pixels
    bool mouse_valid = false;
    float wheel = 0;
    struct Pad {
        bool connected = false;
        std::uint32_t buttons = 0;                              // bit = PadButton
        std::array<float, kPadAxisCount> axes{};                // sticks -1..1 (+y down), triggers 0..1
    };
    std::array<Pad, kMaxGamepads> pads{};
    int touch_count = 0;
    std::array<RawTouch, kMaxTouches> touches{};
    std::array<char32_t, kMaxTextPerFrame> text{};
    int text_count = 0;
    bool key(Key k) const {
        const auto v = static_cast<std::uint32_t>(k);
        return v < static_cast<std::uint32_t>(kKeyCount) && ((keys_down[v >> 6] >> (v & 63u)) & 1u) != 0;
    }
    void set_key(Key k, bool down) {
        const auto v = static_cast<std::uint32_t>(k);
        if (v >= static_cast<std::uint32_t>(kKeyCount)) return;
        const std::uint64_t bit = std::uint64_t{1} << (v & 63u);
        keys_down[v >> 6] = down ? (keys_down[v >> 6] | bit) : (keys_down[v >> 6] & ~bit);
    }
};
}
