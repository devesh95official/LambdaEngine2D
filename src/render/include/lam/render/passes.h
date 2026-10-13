#pragma once
// Offscreen targets, routed by layer range so the sort key needs no pass bits. A target is
// composited back at a later layer of the same view. Target 0 is the virtual screen.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <lam/core/id.h>
#include <lam/math/color.h>
#include <lam/math/vec2.h>
#include <lam/render/sprite.h>
#include <lam/render/texture.h>
namespace lam {
using RenderTargetId = std::uint8_t;                            // 0 = the virtual screen
// How an offscreen target is sized, sampled when composited, and cleared.
struct RenderTargetDesc {
    IVec2 size;                                                 // 0,0 = virtual size * virtual_scale
    float virtual_scale = 1;
    TextureFilter filter = TextureFilter::Point;
    bool clear = true;                                          // once per view
    Color clear_color = kTransparent;
};
class RenderPasses {                                            // App resource
public:
    // Re-adding a name replaces its description. Returns 1..7.
    RenderTargetId add_target(StrId name, const RenderTargetDesc &d);
    RenderTargetId find(StrId name) const;                      // 0 if unknown
    // Inclusive range; target 0 sends the layers back to the screen.
    void route_layers(std::uint8_t first, std::uint8_t last, RenderTargetId target);
    // Draws `source` with `material` just before the quads of `at_layer`.
    void composite(RenderTargetId source, MaterialId material, std::uint8_t at_layer);
    RenderTargetId target_of(std::uint8_t layer) const;
    struct Composite { RenderTargetId source; MaterialId material; std::uint8_t layer; };
    std::span<const Composite> composites() const;             // sorted by layer
    static constexpr std::size_t kMaxTargets = 7;
    static constexpr std::size_t kMaxComposites = 16;
    std::size_t target_count() const;
    const RenderTargetDesc &desc(RenderTargetId id) const;
    StrId name_of(RenderTargetId id) const;
    // At least 1x1. Target 0 is the virtual screen.
    IVec2 target_size(RenderTargetId id, IVec2 virtual_size) const;
    std::uint32_t version() const;                              // backends re-create targets when it changes
private:
    struct Target { StrId name; RenderTargetDesc desc; };
    std::array<Target, kMaxTargets> targets_{};
    std::array<RenderTargetId, 256> layer_target_{};
    std::array<Composite, kMaxComposites> composites_{};
    std::size_t target_count_ = 0;
    std::size_t composite_count_ = 0;
    std::uint32_t version_ = 0;
};
}
