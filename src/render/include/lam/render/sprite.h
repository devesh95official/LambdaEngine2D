#pragma once
// Sprite components and layers. Layers from kFirstScreenLayer up are screen space.
// The default pivot puts the Transform at the feet, so a motor body wants Collider::offset {0, -half.y}.
#include <cstdint>
#include <lam/ecs/component.h>
#include <lam/math/color.h>
#include <lam/math/rect.h>
#include <lam/render/texture.h>
namespace lam {
using MaterialId = std::uint8_t;                                // index into Materials
inline constexpr std::uint8_t kLayerBackground = 0;
inline constexpr std::uint8_t kLayerTiles = 16;
inline constexpr std::uint8_t kLayerWorld = 64;                 // y-sorted actors
inline constexpr std::uint8_t kLayerForeground = 128;           // canopies, world particles
inline constexpr std::uint8_t kLayerLights = 190;               // routed to the light target when lit
inline constexpr std::uint8_t kFirstScreenLayer = 200;
inline constexpr std::uint8_t kLayerScreenFx = 200;             // weather, screen-space particles
inline constexpr std::uint8_t kLayerUi = 220;
inline constexpr std::uint8_t kLayerOverlay = 240;              // scene fades, toasts
inline constexpr std::uint8_t kLayerDebug = 250;
constexpr bool is_screen_layer(std::uint8_t layer) { return layer >= kFirstScreenLayer; }
enum SpriteFlags : std::uint8_t { kFlipX = 1, kFlipY = 2, kHidden = 4, kYSort = 8 };
struct Sprite {
    TextureRegion region;
    Vec2 pivot{0.5f, 1.0f};                                     // fraction of the frame; may go outside 0..1
    Color tint = kWhite;
    std::uint8_t layer = kLayerWorld;
    std::uint8_t flags = kYSort;
    MaterialId material = 0;
    float depth = 0;                                            // without kYSort; larger draws later
    float sort_offset = 0;                                      // added to y when y-sorting
};
struct SpriteFade { float alpha = 1; float target = 1; float seconds = 0.2f; };   // multiplies tint.a
struct RenderOffset { Vec2 offset; Vec2 scale{1, 1}; float rotation = 0; };       // for frame tweens
struct Shadow { Vec2 radii{6, 3}; Vec2 offset; Color color{0, 0, 0, 90}; };       // ellipse under the sprite
// Fades to faded_alpha while a FadeRevealer stands behind it inside `top` (see fade.h).
struct FadeOccluder { Rect top; float inset = 2; float faded_alpha = 0.35f; };    // top is relative to Transform
struct FadeRevealer {};
}
LAM_REFLECT_FLAGS(lam::Sprite, lam::kSerialize | lam::kSnapshot, region, pivot, tint, layer, flags, material,
                  depth, sort_offset)
LAM_REFLECT_FLAGS(lam::SpriteFade, lam::kSerialize | lam::kSnapshot, alpha, target, seconds)
LAM_REFLECT_FLAGS(lam::RenderOffset, lam::kSnapshot, offset, scale, rotation)
LAM_REFLECT_FLAGS(lam::Shadow, lam::kSerialize | lam::kSnapshot, radii, offset, color)
LAM_REFLECT_FLAGS(lam::FadeOccluder, lam::kSerialize | lam::kSnapshot, top, inset, faded_alpha)
LAM_REFLECT_TAG(lam::FadeRevealer)
