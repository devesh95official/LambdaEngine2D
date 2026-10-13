#pragma once
// Optional 2D lighting. Lights are drawn additively into a low-res "light" target that starts at
// the ambient colour, then multiplied over the scene.
#include <cstdint>
#include <lam/core/id.h>
#include <lam/ecs/component.h>
#include <lam/math/color.h>
#include <lam/math/vec2.h>
#include <lam/render/passes.h>
#include <lam/render/sprite.h>
namespace lam {
// Drawn at the interpolated Transform plus `offset` (not rotated or scaled with it).
struct Light2D {
    Color color{255, 214, 150, 255};
    float radius = 48;                                          // world px where the light ends
    float intensity = 1;                                        // 0..1, scales the colour
    float flicker = 0;                                          // 0..1 amplitude
    std::uint32_t seed = 0;                                     // so lamps don't flicker in step
    Vec2 offset;
};
// `enabled` turns lighting on for the world. RenderConfig::lighting only creates the target early.
struct Lighting {
    bool enabled = false;
    Color ambient = kWhite;                                     // light map base (white = unlit look)
    float resolution_scale = 0.5f;                              // light target size / virtual size
};
// Three detuned sines, in [1 - amount, 1].
float flicker_factor(float t, float amount, std::uint32_t seed);
inline constexpr std::uint8_t kLayerLightComposite = kLayerLights + 9;   // 199
inline constexpr StrId kLightTargetName = str_id("light");
// Flicker also wobbles the radius by up to 6%.
struct LightStamp { float radius = 0; Color color; };
LightStamp light_stamp(const Light2D &l, float time_seconds);
// Creates the light target once and routes the light layers to it. Safe to call again.
RenderTargetId ensure_light_pass(RenderPasses &passes, float resolution_scale);
}
LAM_REFLECT_FLAGS(lam::Light2D, lam::kSerialize | lam::kSnapshot, color, radius, intensity, flicker, seed, offset)
