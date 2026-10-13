#pragma once
// Pooled particles on frame time with the App fx RNG: presentation only, never hashed. Pools share
// one slab per world, and gravity and drag integrate exactly so trails match at any frame rate.
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <lam/core/app.h>
#include <lam/math/color.h>
#include <lam/math/rect.h>
#include <lam/math/rng.h>
#include <lam/math/scalar.h>
#include <lam/particles/components.h>
#include <lam/render/sprite.h>
#include <lam/render/texture.h>
namespace lam {
class RenderQueue;
// Upper bound of EmitterConfig::capacity (a pool never holds more).
inline constexpr std::uint32_t kMaxParticlesPerPool = 65536;
// How EmitterConfig::spawn_area is sampled. A zero-size area is a point; an area with zero width or
// zero height is a line.
enum class SpawnShape : std::uint8_t {
    Rect,                                                       // uniform inside the rect
    Ellipse,                                                    // uniform inside the inscribed ellipse
    Ring,                                                       // on the inscribed ellipse's outline
};
// One emitter or burst. Ranges are [min, max), angles radians (+y down), times seconds, sizes
// pixels. Pools only hold sanitize()d copies.
struct EmitterConfig {                                          // asset ".particles.json" or AssetServer::add
    std::uint32_t capacity = 256;                               // <= 65536
    float rate = 0;                                             // particles/s (continuous)
    std::uint16_t burst = 0;                                    // on activation
    float speed_min = 20, speed_max = 60;
    float angle = 0, spread = kTau;
    float life_min = 0.3f, life_max = 0.6f;
    float size_start = 2, size_end = 0;
    Color color_start = kWhite, color_end = kTransparent;
    Vec2 gravity;
    float drag = 0;
    TextureRegion region;                                       // none -> kCircleSource on the shape page
    std::uint8_t layer = kLayerForeground;                      // >= kFirstScreenLayer: screen space (weather)
    MaterialId material = 0;
    Rect spawn_area;                                            // relative to the emitter; empty = point
    bool local_space = false;                                   // particles follow the emitter
    SpawnShape spawn_shape = SpawnShape::Rect;                  // how spawn_area is sampled
    float rotation_min = 0, rotation_max = 0;                   // initial rotation, radians
    float spin_min = 0, spin_max = 0;                           // radians per second
    bool align_to_velocity = false;                             // the quad's x axis follows the velocity
    float stretch = 0;                                          // seconds: length += speed * stretch (streaks; aligns)
    float sway = 0;                                             // pixels: sideways sine offset (snow), drawn only
    float sway_hz = 0;                                          // sway cycles per second
    float jitter = 0;                                           // px/s per sqrt(s): random velocity noise
    bool y_sort = false;                                        // world layers: depth = particle y + depth
    float depth = 0;                                            // sort depth inside `layer` (world layers)
    float prewarm = 0;                                          // seconds pre-simulated when an emitter starts
};
// Copy of `c` with every value finite and clamped to a sane range. No allocation.
EmitterConfig sanitize(const EmitterConfig &c);
// Queues `count` particles for particles_update to emit at the emitter. Safe from tick systems.
void burst(ParticleEmitter &e, std::uint16_t count);
// Screen-space rain over a 480x270 screen. intensity (0..1) only scales the rate, so it can change
// smoothly; capacity stays at 256.
EmitterConfig rain_preset(float intensity);
// Screen-space snow (slow, swaying flakes); same clamping and fixed capacity (512) as rain.
EmitterConfig snow_preset(float intensity);
// Presets for another virtual screen size (non-finite or tiny sizes -> 480x270).
EmitterConfig rain_preset(float intensity, Vec2 screen_size);
EmitterConfig snow_preset(float intensity, Vec2 screen_size);
// Counters of one ParticlePools (diagnostics, tests).
struct ParticlePoolStats {
    std::uint32_t pools = 0;                                    // live pools (draining included)
    std::uint32_t alive = 0;                                    // live particles
    std::uint32_t reserved = 0;                                 // sum of live pool capacities (<= max_particles)
    std::uint32_t rejected = 0;                                 // create() calls refused (over budget or no slot)
    std::uint32_t replaced = 0;                                 // particles overwritten because their pool was full
    std::uint32_t drawn = 0;                                    // quads pushed by the last draw()
    std::uint32_t culled = 0;                                   // particles skipped by the last draw()
    std::uint32_t slab_size = 0;                                // particle slots allocated (grows to its working size)
};
// A world's particle pools. A full pool overwrites its particles in turn; the shared slab only
// grows until it reaches its working size. Not snapshotted or hashed.
class ParticlePools {                                           // World resource (init_world)
public:
    // One live particle (36 bytes, trivially copyable).
    struct Particle {
        Vec2 position;                                          // world/screen px (local space: relative to the anchor)
        Vec2 velocity;                                          // px/s
        float age = 0, life = 1;                                // seconds; dies when age >= life
        float rotation = 0, spin = 0;                           // radians, radians/s
        float phase = 0;                                        // sway phase, radians
    };
    // max_particles caps the summed pool capacities; max_pools is the number of pool slots.
    explicit ParticlePools(std::uint32_t max_particles = 65536, std::uint32_t max_pools = 256);
    ParticlePools(const ParticlePools &) = delete;
    ParticlePools &operator=(const ParticlePools &) = delete;
    // None if over budget or out of slots. May move the slab, invalidating particles() spans.
    ParticlePoolId create(const EmitterConfig &c);
    // Frees the pool and its particles now. Stale or none ids are ignored.
    void destroy(ParticlePoolId pool);
    // Stop emitting and destroy the pool once its last particle dies, so trails fade out.
    void release(ParticlePoolId pool);
    // True for a live pool (draining pools included).
    bool valid(ParticlePoolId pool) const;
    // Emits `count` particles at `origin` (plus the spawn area). Local-space pools also move their
    // anchor there. Ignored for stale or draining pools.
    void emit(ParticlePoolId pool, Vec2 origin, std::uint16_t count, Rng &rng);
    // Emits `count` particles spread evenly along the emitter's move from `from` to `to`, each
    // pre-aged by its share of `seconds`, so streams look the same at any frame rate.
    void emit_stream(ParticlePoolId pool, Vec2 from, Vec2 to, std::uint32_t count, float seconds, Rng &rng);
    // Anchor for local-space particles; `rotation` turns the emission angle and spawn area.
    void set_origin(ParticlePoolId pool, Vec2 origin, float rotation = 0);
    // Replaces the config. A capacity change moves the pool, keeping what fits; false if that
    // would go over budget. Like create(), invalidates particles() spans.
    bool set_config(ParticlePoolId pool, const EmitterConfig &c);
    // The pool's sanitized config; null for stale ids. Valid until destroy/set_config.
    const EmitterConfig *config(ParticlePoolId pool) const;
    // Kills every particle of one pool (stale ids ignored), or of every pool.
    void clear(ParticlePoolId pool);
    void clear();
    // Ages every particle by dt (gravity and drag integrated exactly) and destroys drained pools.
    void update(float dt, Rng &rng);
    // One quad per visible particle; world layers are culled against visible_world.
    void draw(RenderQueue &q, const Rect &visible_world) const;
    // Same, but screen layers are also culled against visible_screen (virtual px).
    void draw(RenderQueue &q, const Rect &visible_world, const Rect &visible_screen) const;
    // Live particles of a pool, in no particular order. Invalidated by create() or set_config() on
    // any pool, and by most calls on this one.
    std::span<const Particle> particles(ParticlePoolId pool) const;
    // Live particles of one pool (0 for stale ids), and of every pool.
    std::uint32_t alive(ParticlePoolId pool) const;
    std::uint32_t total_alive() const;
    // Capacity of a live pool; 0 for stale ids.
    std::uint32_t capacity(ParticlePoolId pool) const;
    // The budget and the number of pool slots given to the constructor.
    std::uint32_t max_particles() const { return max_particles_; }
    std::uint32_t max_pools() const { return static_cast<std::uint32_t>(slots_.size()); }
    // Counters; drawn/culled refer to the last draw().
    ParticlePoolStats stats() const;
private:
    struct Slot {
        EmitterConfig config;
        Vec2 origin;
        float rotation = 0;
        Vec2 bounds_min, bounds_max;                            // particle positions (local for local_space)
        float max_speed = 0;                                    // for stretched quads' cull margin
        std::uint32_t generation = 0;                           // 0 = never used
        std::uint32_t offset = 0, capacity = 0, count = 0, next_replace = 0;
        bool live = false, draining = false;
    };
    Slot *get(ParticlePoolId pool);
    const Slot *get(ParticlePoolId pool) const;
    bool reserve_range(std::uint32_t slot_index, std::uint32_t capacity, bool keep_particles);
    void compact(std::uint32_t last_slot);
    void free_slot(std::uint32_t slot_index);
    Particle make_particle(const Slot &s, Vec2 origin, Rng &rng) const;
    void store(Slot &s, const Particle &p);
    static void advance(Particle &p, const Slot &s, float dt);
    void draw_slot(RenderQueue &q, const Slot &s, const Rect &view) const;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;                           // free slot indices; back() first
    std::vector<std::uint32_t> order_;                          // compaction scratch (max_pools)
    std::vector<Particle> slab_;
    std::uint32_t top_ = 0;                                     // slab bump pointer
    std::uint32_t reserved_ = 0;
    std::uint32_t max_particles_ = 0;
    std::uint32_t rejected_ = 0, replaced_ = 0;
    mutable std::uint32_t drawn_ = 0, culled_ = 0;
};
// ParticlesPlugin options: the ParticlePools constructor arguments used for every world.
struct ParticlesConfig { std::uint32_t max_particles_per_world = 65536; std::uint32_t max_pools = 256; };
// Registers the particle components, the .particles.json loader, BurstAt, particles_update
// (Update -1000) and draw_particles (Render -800). Add it after AssetsPlugin.
class ParticlesPlugin final : public Plugin {
public:
    explicit ParticlesPlugin(ParticlesConfig c = {});
    std::string_view name() const override { return "lam.particles"; }
    void build(App &app) override;                              // loader, BurstAt, systems
    void init_world(App &app, World &world) override;           // ParticlePools; restore hook
private:
    ParticlesConfig config_;
};
// The plugin's systems, public for tests and custom schedules.
void particles_update(Ctx &ctx);                                // Update PerWorld -1000
void draw_particles(Ctx &ctx);                                  // Render PerWorld -800
}
