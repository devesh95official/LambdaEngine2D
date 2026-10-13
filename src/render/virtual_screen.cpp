// Virtual canvas scaling. PixelExpand is the old engine's mode: integer scale from the height,
// with the canvas grown to cover the framebuffer.
#include <algorithm>
#include <cmath>
#include <optional>

#include <lam/render/virtual_screen.h>

namespace lam {
namespace {

// Halves round up; saturates at 1e9.
int round_to_int(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0e9f) return 1000000000;
    return static_cast<int>(std::floor(v + 0.5f));
}

// Negative offset when size > outer (cropped).
int centre_offset(int outer, int size) {
    return (outer - size) / 2;
}

} // namespace

VirtualLayout compute_virtual_layout(IVec2 framebuffer, IVec2 target_virtual, ScaleMode mode) {
    VirtualLayout l;
    l.framebuffer = {std::max(framebuffer.x, 1), std::max(framebuffer.y, 1)};
    const IVec2 target{std::max(target_virtual.x, 1), std::max(target_virtual.y, 1)};
    const IVec2 fb = l.framebuffer;

    switch (mode) {
    case ScaleMode::PixelExpand: {
        // round(fb.y / target.y) in integers.
        l.scale = std::max(1, (fb.y + target.y / 2) / target.y);
        l.virtual_size = {(fb.x + l.scale - 1) / l.scale, (fb.y + l.scale - 1) / l.scale};
        l.scale_xy = {static_cast<float>(l.scale), static_cast<float>(l.scale)};
        // Anchored at the origin, cropped at the right and bottom.
        l.viewport = {0, 0, l.virtual_size.x * l.scale, l.virtual_size.y * l.scale};
        break;
    }
    case ScaleMode::PixelPerfect: {
        // Largest integer scale that fits; never below 1, so tiny windows crop.
        l.scale = std::max(1, std::min(fb.x / target.x, fb.y / target.y));
        l.virtual_size = target;
        l.scale_xy = {static_cast<float>(l.scale), static_cast<float>(l.scale)};
        const IVec2 size{target.x * l.scale, target.y * l.scale};
        l.viewport = {centre_offset(fb.x, size.x), centre_offset(fb.y, size.y), size.x, size.y};
        break;
    }
    case ScaleMode::Fit: {
        const float sx = static_cast<float>(fb.x) / static_cast<float>(target.x);
        const float sy = static_cast<float>(fb.y) / static_cast<float>(target.y);
        const float s = std::min(sx, sy);
        const IVec2 size{std::max(1, round_to_int(static_cast<float>(target.x) * s)),
                         std::max(1, round_to_int(static_cast<float>(target.y) * s))};
        l.virtual_size = target;
        l.scale = std::max(1, static_cast<int>(std::floor(s)));
        l.scale_xy = {static_cast<float>(size.x) / static_cast<float>(target.x),
                      static_cast<float>(size.y) / static_cast<float>(target.y)};
        l.viewport = {centre_offset(fb.x, size.x), centre_offset(fb.y, size.y), size.x, size.y};
        break;
    }
    case ScaleMode::Stretch: {
        l.virtual_size = target;
        l.scale_xy = {static_cast<float>(fb.x) / static_cast<float>(target.x),
                      static_cast<float>(fb.y) / static_cast<float>(target.y)};
        l.scale = std::max(1, static_cast<int>(std::floor(std::min(l.scale_xy.x, l.scale_xy.y))));
        l.viewport = {0, 0, fb.x, fb.y};
        break;
    }
    }
    return l;
}

std::optional<Vec2> framebuffer_to_virtual(const VirtualLayout &l, Vec2 fb_pos) {
    if (!is_finite(fb_pos)) return std::nullopt;
    // PixelExpand's viewport can overhang the framebuffer, so check both.
    const float x0 = static_cast<float>(std::max(l.viewport.x, 0));
    const float y0 = static_cast<float>(std::max(l.viewport.y, 0));
    const float x1 = static_cast<float>(std::min(l.viewport.x + l.viewport.w, l.framebuffer.x));
    const float y1 = static_cast<float>(std::min(l.viewport.y + l.viewport.h, l.framebuffer.y));
    if (fb_pos.x < x0 || fb_pos.y < y0 || fb_pos.x >= x1 || fb_pos.y >= y1) return std::nullopt;
    if (!(l.scale_xy.x > 0.0f) || !(l.scale_xy.y > 0.0f)) return std::nullopt;
    return Vec2{(fb_pos.x - static_cast<float>(l.viewport.x)) / l.scale_xy.x,
                (fb_pos.y - static_cast<float>(l.viewport.y)) / l.scale_xy.y};
}

Vec2 virtual_to_framebuffer(const VirtualLayout &l, Vec2 virtual_pos) {
    return {static_cast<float>(l.viewport.x) + virtual_pos.x * l.scale_xy.x,
            static_cast<float>(l.viewport.y) + virtual_pos.y * l.scale_xy.y};
}

} // namespace lam
