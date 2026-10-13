#include <lam/math/transform.h>

#include <cmath>

namespace lam {

Mat2x3 Mat2x3::trs(Vec2 t, float rotation, Vec2 scale) {
    const float s = det_sin(rotation);
    const float c = det_cos(rotation);
    return {c * scale.x, s * scale.x, -s * scale.y, c * scale.y, t.x, t.y};
}

Mat2x3 Mat2x3::inverse() const {
    const float det = a * d - b * c;
    if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return identity();
    const float inv = 1.0f / det;
    const float ia = d * inv, ib = -b * inv, ic = -c * inv, id = a * inv;
    return {ia, ib, ic, id, -(ia * tx + ic * ty), -(ib * tx + id * ty)};
}

Mat2x3 Transform2D::matrix() const { return Mat2x3::trs(position, rotation, scale); }

Transform2D lerp(const Transform2D &a, const Transform2D &b, float t) {
    const float delta = wrap_angle(b.rotation - a.rotation);
    return {lerp(a.position, b.position, t), a.rotation + delta * t, lerp(a.scale, b.scale, t)};
}

Transform2D combine(const Transform2D &parent, const Transform2D &local) {
    return {parent.matrix().apply(local.position), parent.rotation + local.rotation,
            parent.scale * local.scale};
}

} // namespace lam
