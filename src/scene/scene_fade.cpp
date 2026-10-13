// scene_fade (RenderUI Once +1000): one full-screen quad at kLayerOverlay for the running fade.
// It runs after every world's RenderUI pass, so it sorts above all of them.
#include <cmath>

#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include <lam/render/virtual_screen.h>
#include <lam/scene/scene.h>

namespace lam::detail {

void scene_fade_system(Ctx &ctx) {
    const SceneStack *stack = ctx.try_res<SceneStack>();
    if (!stack) return;
    const float alpha = stack->fade_alpha();
    if (!(alpha > 0.0f)) return;
    RenderQueue *queue = ctx.try_res<RenderQueue>();
    if (!queue) return;
    IVec2 size = ctx.app().config().virtual_size;
    if (const VirtualLayout *layout = ctx.try_res<VirtualLayout>();
        layout && layout->virtual_size.x > 0 && layout->virtual_size.y > 0)
        size = layout->virtual_size;
    Color c = stack->fade_color();
    c.a = static_cast<std::uint8_t>(std::lround(static_cast<float>(c.a) * alpha));
    if (c.a == 0) return;
    queue->rect(Rect{0, 0, static_cast<float>(size.x), static_cast<float>(size.y)}, c, kLayerOverlay);
}

} // namespace lam::detail
