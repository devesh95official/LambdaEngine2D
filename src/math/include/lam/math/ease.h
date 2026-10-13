#pragma once
// Easing curves. Each one starts at 0 and ends at 1; t is clamped to [0, 1] first.
#include <cstdint>
#include <optional>
#include <string_view>
namespace lam {
enum class Ease : std::uint8_t { Linear, InQuad, OutQuad, InOutQuad, InCubic, OutCubic, InOutCubic,
    InSine, OutSine, InOutSine, InExpo, OutExpo, InOutExpo, InBack, OutBack, InOutBack,
    OutBounce, OutElastic, Step };
using EaseFn = float (*)(float);
// The curve's value at t (clamped to [0, 1]).
float ease(Ease e, float t);
EaseFn ease_fn(Ease e);
// "out_quad" -> Ease::OutQuad; unknown -> nullopt.
std::optional<Ease> ease_from_name(std::string_view name);
std::string_view ease_name(Ease e);
}
