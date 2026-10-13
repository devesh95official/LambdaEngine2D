// Extra shapes over RenderQueue::push. See lam/render/shapes.h.
#include <lam/render/shapes.h>

#include <lam/math/scalar.h>
#include <lam/render/texture.h>

namespace lam {

void draw_triangle(RenderQueue &q, Vec2 a, Vec2 b, Vec2 c, Color color, std::uint8_t layer,
                   float depth) {
    if (!is_finite(a) || !is_finite(b) || !is_finite(c)) return;
    const float area2 = cross(b - a, c - a);
    if (!(area2 != 0.0f) || !is_finite(area2)) return;
    // Texels inside the white block, so every fragment samples opaque white.
    const Rect &s = kWhiteSource;
    const Vec2 tl{s.x, s.y}, tr{s.x + s.w, s.y}, br{s.x + s.w, s.y + s.h};
    const QuadVertex v[4] = {{a, tl, color}, {b, tr, color}, {c, br, color}, {c, br, color}};
    q.push(layer, depth, 0, q.shape_texture(), v);
}

// RenderQueue's forwarding member.
void RenderQueue::triangle(Vec2 a, Vec2 b, Vec2 c, Color col, std::uint8_t layer, float depth) {
    draw_triangle(*this, a, b, c, col, layer, depth);
}

} // namespace lam
