// ParticlesPlugin and its systems. The pool handle lives in the unreflected ParticleRuntime, so a
// restore drops it and particles_update gets a new pool. Removing it lets the old pool fade out.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <lam/assets/asset_server.h>
#include <lam/core/log.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/render_queue.h>

#include "detail/particles_detail.h"

namespace lam {
namespace {

using detail::ParticleBook;

constexpr float kHuge = 1.0e30f;
constexpr Rect kEverything{-kHuge, -kHuge, 2 * kHuge, 2 * kHuge};
constexpr float kMaxFrameSeconds = 1.0f;                       // longer frames are clamped

// ParticleRuntime on_remove: the pool drains (stops emitting, dies with its last particle).
void runtime_removed(World &w, Entity, void *component) {
    const ParticleRuntime &rt = *static_cast<const ParticleRuntime *>(component);
    if (ParticlePools *pools = w.resources().try_get<ParticlePools>()) pools->release(rt.pool);
    if (ParticleBook *book = w.resources().try_get<ParticleBook>()) {
        if (rt.pool.index < book->owners.size() && book->owners[rt.pool.index].pool == rt.pool)
            book->owners[rt.pool.index] = {};
    }
}

// Restore hook: restore prunes ParticleRuntime without firing hooks, so release every emitter
// pool whose owner no longer holds it. Survivors keep their pools; the rest re-acquire.
void after_restore(World &w) {
    ParticlePools *pools = w.resources().try_get<ParticlePools>();
    ParticleBook *book = w.resources().try_get<ParticleBook>();
    if (pools == nullptr || book == nullptr) return;
    for (ParticleBook::Owner &o : book->owners) {
        if (!o.pool) continue;
        const ParticleRuntime *rt = w.alive(o.entity) ? w.try_get<ParticleRuntime>(o.entity) : nullptr;
        if (rt != nullptr && rt->pool == o.pool) {
            o.snap = true; // a restore moves entities: the next stream starts where the emitter is
            continue;
        }
        pools->release(o.pool);
        o = {};
    }
}

float frame_seconds(const Ctx &ctx) {
    const float dt = ctx.time().frame_dt;
    if (!(dt > 0.0f) || !std::isfinite(dt)) return 0.0f;
    return std::min(dt, kMaxFrameSeconds);
}

// Pre-simulates `prewarm` seconds of the continuous stream (a level that starts in the rain).
void prewarm(ParticlePools &pools, ParticlePoolId id, const EmitterConfig &c, Vec2 origin, Rng &rng) {
    if (!(c.prewarm > 0.0f) || !(c.rate > 0.0f)) return;
    // Births older than life_max are dead anyway, and more than twice the capacity would only
    // overwrite each other: shrink the window, keeping the density.
    float window = std::min(c.prewarm, c.life_max);
    const float most = 2.0f * static_cast<float>(c.capacity);
    if (c.rate * window > most) window = most / c.rate;
    const auto n = static_cast<std::uint32_t>(c.rate * window);
    pools.emit_stream(id, origin, origin, n, window, rng);
}

// Gives entity `e` (ParticleEmitter, no ParticleRuntime) a pool, if its config is Ready and the
// budget allows. Structural: call outside views.
void acquire(World &w, ParticlePools &pools, ParticleBook &book, AssetServer &assets, Entity e,
             float alpha, Rng &rng) {
    const ParticleEmitter &em = w.get<ParticleEmitter>(e);
    const EmitterConfig *cfg = assets.get(em.config);
    if (cfg == nullptr) return;
    const ParticlePoolId id = pools.create(*cfg);
    if (!id) {
        if (!book.warned_budget) {
            LAM_LOG_WARN("particles: pool budget exhausted ({} particles, {} pools); emitters wait",
                         pools.max_particles(), pools.max_pools());
            book.warned_budget = true;
        }
        return;
    }
    const Transform t = interpolated(w, e, alpha);
    pools.set_origin(id, t.position, t.rotation);
    ParticleBook::Owner &o = book.owners[id.index];
    o.entity = e;
    o.pool = id;
    o.config_bits = em.config.bits();
    o.config_version = assets.version(em.config);
    o.last_origin = t.position;
    o.was_active = false;
    o.snap = false;
    o.next_frame = 0;
    const bool active = em.active;
    w.add<ParticleRuntime>(e, ParticleRuntime{id, 0.0f});
    if (active) prewarm(pools, id, *pools.config(id), t.position, rng);
}

// The shared pool that serves BurstAt for `config` (world space), created on first use and
// updated when the asset changes. None if the budget is exhausted.
ParticlePoolId shared_pool(ParticlePools &pools, ParticleBook &book, const AssetServer &assets,
                           AssetId<EmitterConfig> config, const EmitterConfig &cfg) {
    const std::uint64_t bits = config.bits();
    const std::uint32_t version = assets.version(config);
    for (ParticleBook::Shared &s : book.shared) {
        if (s.config_bits != bits) continue;
        if (pools.valid(s.pool)) {
            if (s.config_version != version) {
                EmitterConfig c = cfg;
                c.local_space = false;
                if (pools.set_config(s.pool, c)) s.config_version = version;
            }
            return s.pool;
        }
        EmitterConfig c = cfg;
        c.local_space = false;
        s.pool = pools.create(c);
        s.config_version = version;
        return s.pool;
    }
    if (book.shared.size() >= book.shared.capacity()) return {};
    EmitterConfig c = cfg;
    c.local_space = false;
    const ParticlePoolId id = pools.create(c);
    if (id) book.shared.push_back({bits, version, id});
    return id;
}

bool has_pools(const Ctx &ctx) {
    return ctx.try_local<ParticlePools>() != nullptr && ctx.try_local<ParticleBook>() != nullptr;
}

// Runs before settle_spawned removes JustSpawned, so an emitter teleported or spawned by a tick
// starts its next stream at the new position instead of streaking along the jump.
void particles_mark_spawned(Ctx &ctx) {
    World &w = ctx.world();
    ParticleBook &book = ctx.local<ParticleBook>();
    for (auto [e, rt] : w.view<ParticleRuntime>()) {
        if (!w.has<JustSpawned>(e)) continue;
        if (rt.pool.index < book.owners.size() && book.owners[rt.pool.index].pool == rt.pool)
            book.owners[rt.pool.index].snap = true;
    }
}

} // namespace

void particles_update(Ctx &ctx) {
    World &w = ctx.world();
    ParticlePools *pools = ctx.try_local<ParticlePools>();
    ParticleBook *book = ctx.try_local<ParticleBook>();
    if (pools == nullptr || book == nullptr) return;
    AssetServer *assets = ctx.try_res<AssetServer>();
    const float dt = frame_seconds(ctx);
    const float alpha = ctx.time().alpha;
    Rng &rng = ctx.fx_rng();

    // 1. Runtimes whose ParticleEmitter is gone (removed, or not restored): release their pools.
    book->scratch.clear();
    for (auto [e, rt] : w.view<ParticleRuntime>(Exclude<ParticleEmitter>{})) {
        (void)rt;
        book->scratch.push_back(e);
    }
    for (const Entity e : book->scratch)
        w.remove<ParticleRuntime>(e);

    // 2. Age and move every particle (also finishes drained pools).
    pools->update(dt, rng);

    // 3. New emitters (and emitters whose pool was lost) acquire a pool once their config is Ready.
    if (assets != nullptr) {
        book->scratch.clear();
        for (auto [e, em] : w.view<ParticleEmitter>(Exclude<ParticleRuntime>{})) {
            if (assets->get(em.config) != nullptr) book->scratch.push_back(e);
        }
        for (const Entity e : book->scratch)
            acquire(w, *pools, *book, *assets, e, alpha, rng);
    }

    // 4. Drive emitters: config changes, activation and pending bursts, continuous streams.
    book->scratch.clear();
    for (auto [e, em, rt] : w.view<ParticleEmitter, ParticleRuntime>()) {
        if (!pools->valid(rt.pool) || rt.pool.index >= book->owners.size()) {
            book->scratch.push_back(e); // destroyed behind our back: re-acquire next frame
            continue;
        }
        const Transform t = interpolated(w, e, alpha);
        ParticleBook::Owner &o = book->owners[rt.pool.index];
        if (o.pool != rt.pool) { // ParticleRuntime added by hand: adopt it
            o = {};
            o.entity = e;
            o.pool = rt.pool;
            o.config_bits = em.config.bits();
            o.config_version = assets != nullptr ? assets->version(em.config) : 0;
            o.last_origin = t.position;
        }
        if (assets != nullptr) {
            const std::uint64_t bits = em.config.bits();
            const std::uint32_t version = assets->version(em.config);
            if (bits != o.config_bits || version != o.config_version) {
                // Not Ready (reloading, or a new id still loading): keep the old config for now.
                const EmitterConfig *cfg = assets->get(em.config);
                if (cfg != nullptr && pools->set_config(rt.pool, *cfg)) {
                    o.config_bits = bits;
                    o.config_version = version;
                }
            }
        }
        const EmitterConfig &cfg = *pools->config(rt.pool);
        pools->set_origin(rt.pool, t.position, t.rotation);
        // Spread births along the emitter's path since last frame, unless it jumped (spawned,
        // teleported, restored, or its world was hidden last frame).
        const std::uint64_t frame = ctx.time().frame;
        const bool gap = o.next_frame != 0 && o.next_frame != frame;
        const bool jumped = o.snap || gap || w.has<JustSpawned>(e);
        const Vec2 from = jumped ? t.position : o.last_origin;
        o.snap = false;
        o.next_frame = frame + 1;
        o.last_origin = t.position;
        std::uint32_t count = em.pending_burst;
        em.pending_burst = 0;
        if (em.active && !o.was_active) count += cfg.burst;
        o.was_active = em.active;
        if (count > 0)
            pools->emit(rt.pool, t.position, static_cast<std::uint16_t>(std::min<std::uint32_t>(count, 65535u)),
                        rng);
        if (!em.active || !(cfg.rate > 0.0f)) {
            rt.accumulator = 0.0f;
            continue;
        }
        float acc = rt.accumulator + cfg.rate * dt;
        if (!(acc >= 0.0f) || !(acc < 1.0e9f)) acc = 0.0f;
        const float whole = std::floor(acc);
        rt.accumulator = acc - whole;
        const auto n = static_cast<std::uint32_t>(std::min(whole, static_cast<float>(cfg.capacity)));
        if (n > 0) pools->emit_stream(rt.pool, from, t.position, n, dt, rng);
    }
    for (const Entity e : book->scratch)
        w.remove<ParticleRuntime>(e);

    // 5. BurstAt from simulation code: one shared world-space pool per config asset. Pools of
    // configs that are no longer loaded drain away.
    if (assets == nullptr) return;
    for (std::size_t i = 0; i < book->shared.size();) {
        ParticleBook::Shared &s = book->shared[i];
        if (assets->get(AssetId<EmitterConfig>::from_bits(s.config_bits)) == nullptr || !pools->valid(s.pool)) {
            pools->release(s.pool);
            s = book->shared.back();
            book->shared.pop_back();
            continue;
        }
        ++i;
    }
    for (const BurstAt &b : ctx.read<BurstAt>()) {
        const EmitterConfig *cfg = assets->get(b.config);
        if (cfg == nullptr) continue;
        const ParticlePoolId pool = shared_pool(*pools, *book, *assets, b.config, *cfg);
        const std::uint16_t count = b.count != 0 ? b.count : cfg->burst;
        if (pool && count > 0) pools->emit(pool, b.position, count, rng);
    }
}

void draw_particles(Ctx &ctx) {
    const ParticlePools *pools = ctx.try_local<ParticlePools>();
    RenderQueue *queue = ctx.try_res<RenderQueue>();
    if (pools == nullptr || queue == nullptr) return;
    Rect world = kEverything, screen = kEverything;
    if (const CameraView *view = ctx.try_local<CameraView>()) {
        if (view->valid) world = view->visible_world;
        screen = {0, 0, static_cast<float>(view->virtual_size.x), static_cast<float>(view->virtual_size.y)};
    }
    pools->draw(*queue, world, screen);
}

ParticlesPlugin::ParticlesPlugin(ParticlesConfig c) : config_(c) {}

void ParticlesPlugin::build(App &app) {
    app.require_plugin("lam.assets", name());
    app.register_component<ParticleEmitter>();
    app.register_component<ParticleRuntime>(); // unreflected: runtime, pruned on restore
    ComponentRegistry::global().set_hooks<ParticleRuntime>(nullptr, nullptr, &runtime_removed);
    app.add_event<BurstAt>(EventScope::World, 256);
    if (AssetServer *assets = app.try_res<AssetServer>()) {
        assets->register_loader<EmitterConfig>(detail::make_emitter_loader());
    } else {
        LAM_LOG_ERROR("lam.particles: add AssetsPlugin before ParticlesPlugin (no .particles.json loader)");
    }
    SystemDesc update;
    update.stage = Stage::Update;
    update.fn = &particles_update;
    update.name = "particles_update";
    update.order = -1000;
    update.scope = SystemScope::PerWorld;
    update.run_if = &has_pools;
    app.add_system(update);
    SystemDesc mark;
    mark.stage = Stage::TickEnd;
    mark.fn = &particles_mark_spawned;
    mark.name = "particles_mark_spawned";
    mark.order = 900;
    mark.scope = SystemScope::PerWorld;
    mark.run_if = &has_pools;
    app.add_system(mark);
    SystemDesc draw;
    draw.stage = Stage::Render;
    draw.fn = &draw_particles;
    draw.name = "draw_particles";
    draw.order = -800;
    draw.scope = SystemScope::PerWorld;
    draw.run_if = &has_local<ParticlePools>;
    app.add_system(draw);
}

void ParticlesPlugin::init_world(App &, World &world) {
    world.resources().insert<ParticlePools>(config_.max_particles_per_world, config_.max_pools);
    world.resources().insert<detail::ParticleBook>(std::min<std::uint32_t>(config_.max_pools, 65535u));
    world.add_restore_hook(&after_restore);
}

} // namespace lam
