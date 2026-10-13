#pragma once
// Virtual resolution and how it maps onto the window.
#include <cstdint>
#include <optional>
#include <lam/math/rect.h>
#include <lam/math/vec2.h>
namespace lam {
enum class ScaleMode : std::uint8_t {
    PixelPerfect,                                               // integer scale, fixed virtual size, letterbox
    PixelExpand,                                                // integer scale, virtual size grows to fill
    Fit,                                                        // fractional scale, letterbox
    Stretch,
};
struct VirtualLayout {
    IVec2 framebuffer;
    IVec2 virtual_size;
    int scale = 1;                                              // floored in fractional modes
    Vec2 scale_xy{1, 1};                                        // framebuffer px per virtual px
    IRect viewport;                                             // drawn area in framebuffer pixels
    bool operator==(const VirtualLayout &) const = default;
};
VirtualLayout compute_virtual_layout(IVec2 framebuffer, IVec2 target_virtual, ScaleMode mode);
std::optional<Vec2> framebuffer_to_virtual(const VirtualLayout &l, Vec2 fb_pos);   // nullopt outside viewport
Vec2 virtual_to_framebuffer(const VirtualLayout &l, Vec2 virtual_pos);
}
