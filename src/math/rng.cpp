#include <lam/math/rng.h>

#include <cmath>

namespace lam {

Vec2 Rng::unit_vector() { return from_angle(unit() * kTau); }

Vec2 Rng::in_disc(float radius) {
    const Vec2 dir = unit_vector();
    return dir * (std::sqrt(unit()) * radius);
}

} // namespace lam
