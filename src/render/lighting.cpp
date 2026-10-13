// Light flicker and pass setup. Presentation only, so libm sin is fine.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <lam/math/scalar.h>
#include <lam/render/lighting.h>
#include <lam/render/shader.h>

namespace lam {

float flicker_factor(float t, float amount, std::uint32_t seed) {
    if (!(amount > 0.0f) || !is_finite(amount)) return 1.0f;
    amount = std::min(amount, 1.0f);
    if (!is_finite(t)) t = 0.0f;
    const float phase = static_cast<float>(seed % 1000u) * 0.0137f;
    const float wave = 0.5f * std::sin(t * 7.3f + phase) +
                       0.3f * std::sin(t * 13.1f + phase * 2.1f) +
                       0.2f * std::sin(t * 23.7f + phase * 3.7f);
    // wave is in [-1, 1]; map to [1 - amount, 1].
    const float f = 1.0f - amount * (0.5f + 0.5f * wave);
    return std::clamp(f, 1.0f - amount, 1.0f);
}

LightStamp light_stamp(const Light2D &l, float time_seconds) {
    LightStamp s;
    const float radius = is_finite(l.radius) ? std::max(l.radius, 0.0f) : 0.0f;
    const bool flickers = l.flicker > 0.0f && is_finite(l.flicker);
    s.radius =
        flickers ? radius * (0.94f + 0.06f * flicker_factor(time_seconds, 1.0f, l.seed)) : radius;
    const float intensity = is_finite(l.intensity) ? l.intensity : 0.0f;
    const float strength =
        std::clamp(intensity * flicker_factor(time_seconds, l.flicker, l.seed), 0.0f, 1.0f);
    s.color = l.color.with_alpha(static_cast<std::uint8_t>(std::lround(255.0f * strength)));
    return s;
}

RenderTargetId ensure_light_pass(RenderPasses &passes, float resolution_scale) {
    if (const RenderTargetId existing = passes.find(kLightTargetName); existing != 0)
        return existing;
    RenderTargetDesc d;
    d.virtual_scale = (resolution_scale > 0.0f && is_finite(resolution_scale))
                          ? std::clamp(resolution_scale, 0.0625f, 4.0f)
                          : 0.5f;
    d.filter = TextureFilter::Bilinear;
    d.clear = true;
    d.clear_color = kWhite;
    const RenderTargetId id = passes.add_target(kLightTargetName, d);
    passes.route_layers(kLayerLights, static_cast<std::uint8_t>(kLayerLights + 8), id);
    passes.composite(id, kMaterialMultiply, kLayerLightComposite);
    return id;
}

} // namespace lam
