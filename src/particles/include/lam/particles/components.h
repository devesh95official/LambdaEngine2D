#pragma once
// Particle components and the BurstAt event. Tick systems never touch ParticlePools; they emit
// BurstAt or set ParticleEmitter::pending_burst.
#include <cstdint>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/math/vec2.h>
namespace lam {
struct EmitterConfig;                                           // lam/particles/particles.h (asset)
struct ParticlePoolId {
    std::uint32_t index = 0, generation = 0;                    // 0 = none
    constexpr explicit operator bool() const { return index != 0; }
    constexpr bool operator==(const ParticlePoolId &) const = default;
};
struct ParticleEmitter {                                        // component (presentation)
    AssetId<EmitterConfig> config;
    bool active = true;
    std::uint16_t pending_burst = 0;
};
struct ParticleRuntime { ParticlePoolId pool; float accumulator = 0; };   // runtime component
// Burst at a world position, from any tick stage; count 0 uses the config's `burst`. Dropped if
// the config isn't Ready. Without ParticlesPlugin, register it with app.add_event<BurstAt>().
struct BurstAt { AssetId<EmitterConfig> config; Vec2 position; std::uint16_t count = 0; };   // World event
}
LAM_REFLECT_FLAGS(lam::ParticleEmitter, lam::kSerialize | lam::kSnapshot, config, active, pending_burst)
