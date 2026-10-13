#include <lam/math/color.h>

#include <lam/math/scalar.h>

namespace lam {
namespace {
std::uint8_t mix(std::uint8_t a, std::uint8_t b, float t) {
    const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
    return static_cast<std::uint8_t>(clamp(v + 0.5f, 0.0f, 255.0f));
}
std::uint8_t mul(std::uint8_t a, std::uint8_t b) {
    return static_cast<std::uint8_t>((static_cast<unsigned>(a) * b + 127U) / 255U);
}
} // namespace

Color lerp(Color a, Color b, float t) {
    t = saturate(t);
    return {mix(a.r, b.r, t), mix(a.g, b.g, t), mix(a.b, b.b, t), mix(a.a, b.a, t)};
}

Color multiply(Color a, Color b) { return {mul(a.r, b.r), mul(a.g, b.g), mul(a.b, b.b), mul(a.a, b.a)}; }

Color scale_alpha(Color c, float factor) {
    c.a = static_cast<std::uint8_t>(clamp(static_cast<float>(c.a) * saturate(factor) + 0.5f, 0.0f, 255.0f));
    return c;
}

float luma(Color c) {
    return (0.299f * static_cast<float>(c.r) + 0.587f * static_cast<float>(c.g) +
            0.114f * static_cast<float>(c.b)) / 255.0f;
}

} // namespace lam
