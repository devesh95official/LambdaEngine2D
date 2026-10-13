#pragma once
// lam_particles internals: the loader factory and per-world pool bookkeeping. Not installed.
#include <cstdint>
#include <memory>
#include <vector>
#include <lam/assets/asset_server.h>
#include <lam/ecs/entity.h>
#include <lam/math/vec2.h>
#include <lam/particles/particles.h>
namespace lam::detail {
// Strict JSON loader. An optional "image" is packed into an atlas group at finalize, or kept as
// its own texture.
std::unique_ptr<AssetLoader<EmitterConfig>> make_emitter_loader();
// Which emitter owns which pool, plus the shared BurstAt pool per config. Fixed tables; `scratch`
// only grows when more emitters change state in one frame than ever before.
struct ParticleBook {
    struct Owner {                                              // indexed by ParticlePoolId::index
        Entity entity;                                          // null: no emitter owns the pool
        ParticlePoolId pool;
        std::uint64_t config_bits = 0;                          // AssetId<EmitterConfig>::bits()
        std::uint32_t config_version = 0;
        Vec2 last_origin;                                       // previous frame's emitter position
        bool was_active = false;
        bool snap = false;                                      // jumped: next stream starts here
        std::uint64_t next_frame = 0;                           // last update's frame + 1; 0 none
    };
    struct Shared {                                             // the BurstAt pool of one config
        std::uint64_t config_bits = 0;
        std::uint32_t config_version = 0;
        ParticlePoolId pool;
    };
    explicit ParticleBook(std::uint32_t max_pools) {
        owners.resize(std::size_t{max_pools} + 1);
        shared.reserve(max_pools);
        scratch.reserve(64);
    }
    std::vector<Owner> owners;
    std::vector<Shared> shared;
    std::vector<Entity> scratch;
    bool warned_budget = false;
};
}
