#pragma once
// 8-bit RGBA colour.
#include <cstdint>
namespace lam {
struct Color {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
    // Color::hex(0xRRGGBBAA).
    static constexpr Color hex(std::uint32_t rrggbbaa) {
        return {static_cast<std::uint8_t>(rrggbbaa >> 24), static_cast<std::uint8_t>(rrggbbaa >> 16),
                static_cast<std::uint8_t>(rrggbbaa >> 8), static_cast<std::uint8_t>(rrggbbaa)};
    }
    constexpr Color with_alpha(std::uint8_t alpha) const { return {r, g, b, alpha}; }
    constexpr bool operator==(const Color &) const = default;
};
inline constexpr Color kWhite{255, 255, 255, 255};
inline constexpr Color kBlack{0, 0, 0, 255};
inline constexpr Color kTransparent{0, 0, 0, 0};
// Channel-wise lerp, t clamped to [0, 1], rounded to nearest.
Color lerp(Color a, Color b, float t);
// Channel-wise product (a * b / 255), rounded.
Color multiply(Color a, Color b);
// Alpha times factor; factor clamped to [0, 1].
Color scale_alpha(Color c, float factor);
// Perceived brightness 0..1 (Rec. 601 weights).
float luma(Color c);
}
