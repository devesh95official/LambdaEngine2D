// particles_demo: a torch (fx/ember.particles.json, textured from an atlas-packed PNG) burns at
// the bottom of the screen. A click bursts sparks there: a tick system reads the input and emits
// BurstAt. R toggles rain, S snow (screen-space presets that ease in and out). Until the first
// input, an attract mode bursts every 0.75 s and switches the weather, so --frames runs cover it.
//   ./particles_demo [--frames N] [--hidden] [--headless]
// Generated assets: python3 make_assets.py rewrites assets/fx/ember.png.
#include <cstdio>

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/particles/components.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>

using namespace lam;

namespace {

struct Demo {                                                   // App resource
    AssetId<EmitterConfig> sparks;
    ButtonAction burst, rain, snow;                             // none until InputPlugin is installed
    Entity rain_entity, snow_entity;
    bool attract = true;                                        // until the first click or key press
};

// Startup: camera, torch, and two weather emitters (screen space: no Transform needed).
void spawn(Ctx &ctx) {
    World &w = ctx.world();
    AssetServer &assets = ctx.res<AssetServer>();
    Demo &demo = ctx.res<Demo>();
    demo.sparks = assets.load<EmitterConfig>("fx/sparks.particles.json");
    const Entity cam = w.create();
    w.add<Transform>(cam);
    w.add<Camera2D>(cam, Camera2D{.position = {240, 135}});
    const Entity torch = w.create();
    w.add<Transform>(torch, Transform{.position = {240, 230}});
    w.add<ParticleEmitter>(torch, ParticleEmitter{.config = assets.load<EmitterConfig>("fx/ember.particles.json")});
    demo.rain_entity = w.create();
    w.add<ParticleEmitter>(demo.rain_entity, ParticleEmitter{.config = assets.add(rain_preset(1.0f)), .active = false});
    demo.snow_entity = w.create();
    w.add<ParticleEmitter>(demo.snow_entity, ParticleEmitter{.config = assets.add(snow_preset(0.8f)), .active = false});
}

void toggle(World &w, Entity e) {
    ParticleEmitter &em = w.get<ParticleEmitter>(e);
    em.active = !em.active;
}

// Simulate (every tick): input -> BurstAt and weather switches.
void controls(Ctx &ctx) {
    Demo &demo = ctx.res<Demo>();
    World &w = ctx.world();
    const InputFrame &in = input_frame(ctx);
    const bool clicked = in.is_pressed(demo.burst);
    bool rain = in.is_pressed(demo.rain), snow = in.is_pressed(demo.snow);
    if (clicked || rain || snow) demo.attract = false;
    if (clicked && in.pointer_valid) ctx.emit(BurstAt{demo.sparks, in.pointer_world, 0}); // 0: config burst
    if (demo.attract) {
        const std::uint64_t t = ctx.time().tick;
        if (t % 45 == 1) {
            Rng &rng = ctx.rng(str_id("demo.attract"));
            ctx.emit(BurstAt{demo.sparks, {rng.range(60, 420), rng.range(40, 170)}, 0});
        }
        rain = t % 240 == 120;
        snow = t % 480 == 360;
    }
    if (rain) toggle(w, demo.rain_entity);
    if (snow) toggle(w, demo.snow_entity);
}

} // namespace

int main(int argc, char **argv) {
    App app({.title = "particles_demo", .virtual_size = {480, 270}, .settings_file = ""}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    Demo demo;
    if (ActionMap *map = app.try_res<ActionMap>()) {
        demo.burst = map->button("burst", {ActionMap::mouse(MouseButton::Left)});
        demo.rain = map->button("rain", {ActionMap::key(Key::R)});
        demo.snow = map->button("snow", {ActionMap::key(Key::S)});
    }
    app.insert_resource<Demo>(demo);
    app.add_startup(&spawn, "demo.spawn");
    app.add_system(Stage::Simulate, &controls, "demo.controls");
    const int code = app.run();
    const ParticlePoolStats st = app.main_world().resources().get<ParticlePools>().stats();
    std::printf("particles_demo: %llu frames; %u particles in %u pools (%u of %u reserved); last frame drew %u, "
                "culled %u\n",
                static_cast<unsigned long long>(app.time().frame + 1), st.alive, st.pools, st.reserved,
                app.main_world().resources().get<ParticlePools>().max_particles(), st.drawn, st.culled);
    return code;
}
