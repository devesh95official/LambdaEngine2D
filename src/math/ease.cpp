#include <lam/math/ease.h>

#include <array>
#include <cmath>

#include <lam/math/scalar.h>

namespace lam {
namespace {

float linear(float t) { return t; }
float in_quad(float t) { return t * t; }
float out_quad(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }
float in_out_quad(float t) { return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t); }
float in_cubic(float t) { return t * t * t; }
float out_cubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}
float in_out_cubic(float t) {
    if (t < 0.5f) return 4.0f * t * t * t;
    const float u = 1.0f - t;
    return 1.0f - 4.0f * u * u * u;
}
float in_sine(float t) { return 1.0f - det_cos(t * kPi * 0.5f); }
float out_sine(float t) { return det_sin(t * kPi * 0.5f); }
float in_out_sine(float t) { return 0.5f - 0.5f * det_cos(t * kPi); }
float in_expo(float t) { return t <= 0.0f ? 0.0f : std::exp2(10.0f * t - 10.0f); }
float out_expo(float t) { return t >= 1.0f ? 1.0f : 1.0f - std::exp2(-10.0f * t); }
float in_out_expo(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t < 0.5f ? std::exp2(20.0f * t - 10.0f) * 0.5f : (2.0f - std::exp2(-20.0f * t + 10.0f)) * 0.5f;
}
constexpr float kBack1 = 1.70158f;
constexpr float kBack2 = kBack1 * 1.525f;
float in_back(float t) { return (kBack1 + 1.0f) * t * t * t - kBack1 * t * t; }
float out_back(float t) {
    const float u = t - 1.0f;
    return 1.0f + (kBack1 + 1.0f) * u * u * u + kBack1 * u * u;
}
float in_out_back(float t) {
    if (t < 0.5f) return (4.0f * t * t * ((kBack2 + 1.0f) * 2.0f * t - kBack2)) * 0.5f;
    const float u = 2.0f * t - 2.0f;
    return (u * u * ((kBack2 + 1.0f) * u + kBack2) + 2.0f) * 0.5f;
}
float out_bounce(float t) {
    constexpr float n = 7.5625f;
    constexpr float d = 2.75f;
    if (t < 1.0f / d) return n * t * t;
    if (t < 2.0f / d) {
        t -= 1.5f / d;
        return n * t * t + 0.75f;
    }
    if (t < 2.5f / d) {
        t -= 2.25f / d;
        return n * t * t + 0.9375f;
    }
    t -= 2.625f / d;
    return n * t * t + 0.984375f;
}
float out_elastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    constexpr float c4 = kTau / 3.0f;
    return std::exp2(-10.0f * t) * det_sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}
float step(float t) { return t >= 1.0f ? 1.0f : 0.0f; }

struct Entry {
    EaseFn fn;
    std::string_view name;
};
constexpr std::array<Entry, 19> kTable{{
    {linear, "linear"},           {in_quad, "in_quad"},         {out_quad, "out_quad"},
    {in_out_quad, "in_out_quad"}, {in_cubic, "in_cubic"},       {out_cubic, "out_cubic"},
    {in_out_cubic, "in_out_cubic"}, {in_sine, "in_sine"},       {out_sine, "out_sine"},
    {in_out_sine, "in_out_sine"}, {in_expo, "in_expo"},         {out_expo, "out_expo"},
    {in_out_expo, "in_out_expo"}, {in_back, "in_back"},         {out_back, "out_back"},
    {in_out_back, "in_out_back"}, {out_bounce, "out_bounce"},   {out_elastic, "out_elastic"},
    {step, "step"},
}};

} // namespace

float ease(Ease e, float t) { return ease_fn(e)(saturate(t)); }

EaseFn ease_fn(Ease e) {
    const auto i = static_cast<std::size_t>(e);
    return i < kTable.size() ? kTable[i].fn : linear;
}

std::optional<Ease> ease_from_name(std::string_view name) {
    for (std::size_t i = 0; i < kTable.size(); ++i)
        if (kTable[i].name == name) return static_cast<Ease>(i);
    return std::nullopt;
}

std::string_view ease_name(Ease e) {
    const auto i = static_cast<std::size_t>(e);
    return i < kTable.size() ? kTable[i].name : std::string_view{};
}

} // namespace lam
