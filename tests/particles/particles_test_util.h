#pragma once
// lam_particles test helpers. ParticlesHarness is a headless App with the particles plugin stack.
// A probe right after draw_particles records the queue size before render_clear empties it.
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/particles/components.h>
#include <lam/particles/particles.h>
#include <lam/render/render_plugin.h>
#include <lam/render/render_queue.h>
#include <lam/test/support.h>

namespace lam::test {

// Quads the RenderQueue held right after draw_particles, per frame (App resource).
struct DrawProbe {
    std::size_t quads = 0;
    std::uint32_t frames = 0;
};

inline void probe_queue(Ctx &ctx) {
    DrawProbe &p = ctx.res<DrawProbe>();
    const RenderQueue *q = ctx.try_res<RenderQueue>();
    p.quads = q != nullptr ? q->size() : 0;
    ++p.frames;
}

// Clears the queue at the start of Render so the probe only counts this frame's quads.
inline void clear_queue_first(Ctx &ctx) {
    if (RenderQueue *q = ctx.try_res<RenderQueue>()) q->clear();
}

class ParticlesHarness {
public:
    explicit ParticlesHarness(AssetsConfig assets = {}, ParticlesConfig particles = {}, AppConfig app = {}) : h_(std::move(app)) {
        h_.add<AssetsPlugin>(std::move(assets));
        h_.add<RenderPlugin>();
        h_.add<ParticlesPlugin>(particles);
        if (h_.app().try_res<RenderQueue>() == nullptr) h_.app().insert_resource<RenderQueue>(4096);
        h_.app().insert_resource<DrawProbe>();
        SystemDesc clear;
        clear.stage = Stage::Render;
        clear.fn = &clear_queue_first;
        clear.name = "test.clear_queue";
        clear.order = -3000;
        clear.scope = SystemScope::Once;
        h_.app().add_system(clear);
        SystemDesc probe;
        probe.stage = Stage::Render;
        probe.fn = &probe_queue;
        probe.name = "test.probe_queue";
        probe.order = 500;
        probe.scope = SystemScope::Once;
        h_.app().add_system(probe);
    }
    App &app() { return h_.app(); }
    World &world() { return h_.app().main_world(); }
    AssetServer &assets() { return h_.app().res<AssetServer>(); }
    ParticlePools &pools() { return world().resources().get<ParticlePools>(); }
    std::size_t last_quads() { return h_.app().res<DrawProbe>().quads; }
    void frames(int n) { h_.frames(n); }
    // An entity at `pos` with Transform and a ParticleEmitter for `config` (added as an asset).
    Entity emitter(const EmitterConfig &config, Vec2 pos = {}, bool active = true) {
        const AssetId<EmitterConfig> id = assets().add(config);
        return emitter_for(id, pos, active);
    }
    Entity emitter_for(AssetId<EmitterConfig> id, Vec2 pos = {}, bool active = true) {
        World &w = world();
        const Entity e = w.create();
        w.add<Transform>(e, Transform{.position = pos});
        w.add<ParticleEmitter>(e, ParticleEmitter{.config = id, .active = active});
        return e;
    }
    ParticlePoolId pool_of(Entity e) {
        const ParticleRuntime *rt = world().try_get<ParticleRuntime>(e);
        return rt != nullptr ? rt->pool : ParticlePoolId{};
    }

private:
    HeadlessApp h_;
};

// A config with fixed life and no randomness in speed, for exact counting.
inline EmitterConfig steady_config(std::uint32_t capacity = 256, float rate = 0, float life = 1.0f) {
    EmitterConfig c;
    c.capacity = capacity;
    c.rate = rate;
    c.life_min = c.life_max = life;
    c.speed_min = c.speed_max = 10;
    c.size_start = c.size_end = 2;
    c.color_start = c.color_end = kWhite;
    return c;
}

// PNG signature plus an IHDR chunk. The headless decoder only reads the size.
inline std::string png_header_bytes(int w, int h) {
    std::string s = std::string("\x89PNG\r\n\x1a\n", 8);
    const auto be32 = [&s](std::uint32_t v) {
        s.push_back(static_cast<char>((v >> 24) & 0xFF));
        s.push_back(static_cast<char>((v >> 16) & 0xFF));
        s.push_back(static_cast<char>((v >> 8) & 0xFF));
        s.push_back(static_cast<char>(v & 0xFF));
    };
    be32(13);
    s += "IHDR";
    be32(static_cast<std::uint32_t>(w));
    be32(static_cast<std::uint32_t>(h));
    s.push_back(8);  // bit depth
    s.push_back(6);  // RGBA
    s.push_back(0);
    s.push_back(0);
    s.push_back(0);
    be32(0); // CRC (not checked by the header reader)
    return s;
}

} // namespace lam::test
