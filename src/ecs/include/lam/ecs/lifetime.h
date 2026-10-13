#pragma once
// Auto-destroy after N ticks (bullets, effects). Counted down at TickEnd -900 by
// tick_lifetimes() in lam/ecs/transform.h.
#include <cstdint>
#include <lam/ecs/component.h>
namespace lam {
// Ticks left. At 0 the entity and its descendants are destroyed.
struct Lifetime { std::uint32_t ticks = 60; };
}
LAM_REFLECT(lam::Lifetime, ticks)
