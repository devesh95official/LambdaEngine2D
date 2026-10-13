#pragma once
// Sprite fades and fading occluders (from the old depth module). An occluder fades while a
// revealer's feet are behind it and inside its top rect.
#include <lam/math/rect.h>
#include <lam/math/vec2.h>
#include <lam/render/sprite.h>
namespace lam {
constexpr Rect occluder_rect(const FadeOccluder &o, Vec2 position) { return o.top.translated(position); }
// Behind the occluder and inside `top` shrunk by `inset` on the left, right and top, so brushing
// the edge of a canopy doesn't fade it.
bool hidden_behind(const Rect &top, float inset, float occluder_feet_y, Vec2 feet);
// Linear: a full 0..1 change takes `seconds`. seconds <= 0 jumps straight to the target.
float step_fade(float alpha, float target, float seconds, float dt);
}
