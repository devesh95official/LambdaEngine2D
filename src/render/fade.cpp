// Canopy test from the old depth module, and a linear fade.
#include <algorithm>
#include <cmath>

#include <lam/math/scalar.h>
#include <lam/render/fade.h>

namespace lam {

bool hidden_behind(const Rect &top, float inset, float occluder_feet_y, Vec2 feet) {
    // Level with or in front of the object, the revealer is already drawn on top.
    if (!(feet.y < occluder_feet_y)) return false;
    return feet.x >= top.x + inset && feet.x < top.x + top.w - inset && feet.y >= top.y + inset &&
           feet.y < top.y + top.h;
}

float step_fade(float alpha, float target, float seconds, float dt) {
    const float goal = is_finite(target) ? std::clamp(target, 0.0f, 1.0f) : 1.0f;
    // Also recovers from a NaN alpha.
    float current = is_finite(alpha) ? std::clamp(alpha, 0.0f, 1.0f) : goal;
    if (!(seconds > 0.0f) || !is_finite(seconds)) return goal;
    if (!(dt > 0.0f) || !is_finite(dt)) return current;
    const float step = dt / seconds;
    current = goal > current ? std::min(goal, current + step) : std::max(goal, current - step);
    return current;
}

} // namespace lam
