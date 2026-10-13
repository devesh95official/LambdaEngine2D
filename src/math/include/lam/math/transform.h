#pragma once
// Affine 2D transform maths. The ECS component Transform (lam/ecs/transform.h) has the same fields
// and converts with to_transform2d().
#include <lam/math/vec2.h>
namespace lam {
// Column-major affine matrix [a c tx; b d ty].
struct Mat2x3 {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    static constexpr Mat2x3 identity() { return {}; }
    // Translate * rotate * scale; uses det_sin/det_cos.
    static Mat2x3 trs(Vec2 t, float rotation, Vec2 scale);
    constexpr Vec2 apply(Vec2 p) const { return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty}; }
    constexpr Vec2 apply_dir(Vec2 v) const { return {a * v.x + c * v.y, b * v.x + d * v.y}; }
    // Singular or non-finite -> identity.
    Mat2x3 inverse() const;
};
constexpr Mat2x3 operator*(const Mat2x3 &l, const Mat2x3 &r) {
    return {l.a * r.a + l.c * r.b,         l.b * r.a + l.d * r.b,         l.a * r.c + l.c * r.d,
            l.b * r.c + l.d * r.d,         l.a * r.tx + l.c * r.ty + l.tx, l.b * r.tx + l.d * r.ty + l.ty};
}
// Position, rotation (radians) and scale.
struct Transform2D {
    Vec2 position;
    float rotation = 0;
    Vec2 scale{1, 1};
    Mat2x3 matrix() const;
};
// Lerps position and scale; rotation takes the shortest arc.
Transform2D lerp(const Transform2D &a, const Transform2D &b, float t);
// parent * local: local's position is rotated and scaled by the parent; rotations add, scales multiply.
Transform2D combine(const Transform2D &parent, const Transform2D &local);
}
