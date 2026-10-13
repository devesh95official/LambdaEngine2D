#pragma once
// Extra shapes on top of RenderQueue. They use the shape page's white block, so they batch with
// sprites and text.
//   draw_triangle(ctx.res<RenderQueue>(), {0, 0}, {8, 16}, {-8, 16}, Color{255, 80, 80, 255},
//                 kLayerUi);
#include <cstdint>
#include <lam/math/color.h>
#include <lam/math/vec2.h>
#include <lam/render/render_queue.h>
namespace lam {
// Any winding. Drawn as a quad with `c` repeated, so it doesn't break the batch.
void draw_triangle(RenderQueue &q, Vec2 a, Vec2 b, Vec2 c, Color color, std::uint8_t layer,
                   float depth = 0);
}
