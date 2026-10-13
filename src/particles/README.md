# lam_particles

Pooled, frame-time particles. Each world holds a `ParticlePools` resource. Pools have fixed
capacity, remove dead particles by swap-remove, and are carved from one slab per world, so the
frame path does not allocate in steady state. Every config passes through `sanitize()` first:
values are made finite and clamped, and capacity is at most 65536. Gravity and exponential
drag are integrated exactly, and continuous streams are pre-aged along the emitter's path, so a
trail looks the same at 30 fps and at 144 fps. Particles are drawn as textured quads through the
`RenderQueue` (atlas regions or the built-in shapes), so they batch with sprites, and they are
culled against the world's `CameraView`. They are presentation only: they use the App fx RNG and
are never snapshotted or hashed.

## Usage

```cpp
app.add_plugins(DefaultPlugins{});                 // AssetsPlugin, RenderPlugin, ..., ParticlesPlugin
AssetServer &assets = app.res<AssetServer>();
// An emitter that follows its entity (interpolated Transform; rotation turns `angle`).
auto smoke = assets.load<EmitterConfig>("fx/smoke.particles.json");
Entity chimney = w.create();
w.add<Transform>(chimney, Transform{.position = {120, 40}});
w.add<ParticleEmitter>(chimney, ParticleEmitter{.config = smoke});
// Simulation code never touches ParticlePools. It emits BurstAt, or sets pending_burst.
ctx.emit(BurstAt{.config = sparks, .position = hit_point, .count = 0});   // 0: the config's burst
burst(w.get<ParticleEmitter>(chimney), 8);
// Weather: a screen-space preset. It needs no Transform. Toggle it with `active`.
w.add<ParticleEmitter>(w.create(), ParticleEmitter{.config = assets.add(rain_preset(0.7f))});
```

`.particles.json` uses `EmitterConfig`'s member names as keys. Angles are in radians; unknown keys
and out-of-range values are load errors.

```json
{"capacity": 256, "rate": 70, "speed_min": 12, "speed_max": 34, "angle": -1.5708, "spread": 0.7,
 "life_min": 0.8, "life_max": 1.6, "size_start": 6, "size_end": 1,
 "color_start": "#ffd080ff", "color_end": "#ff300000", "gravity": [0, -20], "drag": 0.8,
 "spawn_shape": "ellipse", "spawn_area": [-6, -3, 12, 6], "layer": "foreground",
 "image": "ember.png", "source": [0, 0, 8, 8], "atlas": "fx"}
```

The look of a particle comes from one of two keys:
- `"sprite"`: one of the built-in shapes `circle` (the default), `square` or `falloff`.
- `"image"`: a PNG next to the JSON file. At finalize it is packed into the atlas group named by
  `"atlas"` (default `"default"`). With `"none"`, or when no `AtlasSet` exists, the image becomes
  a standalone texture instead.

Other keys:
- `layer` takes a number, or one of `background`, `tiles`, `world`, `foreground`, `lights`,
  `screen_fx`, `ui`, `overlay`, `debug`. Layers from `screen_fx` (200) up are screen space.
- Also optional: `spawn_shape` (`rect`, `ellipse`, `ring`),
  `rotation_min`/`rotation_max`, `spin_min`/`spin_max`, `align_to_velocity`, `stretch`,
  `sway`/`sway_hz`, `jitter`, `y_sort`, `depth` and `prewarm`.

## Components, resources, events, systems

| Kind | Name | Scope / flags | Notes |
|---|---|---|---|
| Component | `ParticleEmitter` | S N (presentation, never hashed) | `config`, `active`, `pending_burst`. Tick systems may set any field. |
| Component | `ParticleRuntime` | runtime (unreflected) | The pool handle and the stream accumulator. Restore prunes it, and the emitter then re-acquires a pool. Removing it releases the pool, which drains: it emits nothing more and is destroyed when its last particle dies. |
| Resource | `ParticlePools` | World (`ParticlesPlugin::init_world`) | Budget `ParticlesConfig::max_particles_per_world` (65536) is the sum of pool capacities; at most `max_pools` (256) pools. Not snapshotted. |
| Asset | `EmitterConfig` | `.particles.json`, or `AssetServer::add` | Sanitized when it enters a pool. |
| Event | `BurstAt` | World | Emitted by any tick stage, consumed by `particles_update`. Each config asset gets one shared world-space pool. |
| System | `particles_update` | Update, PerWorld, -1000 (`run_if` pools) | In order: release pools of runtimes that lost their emitter; age particles (`frame_dt`, fx RNG); acquire pools for new emitters whose config is Ready; apply config changes; emit activation bursts, `pending_burst` and pre-aged streams at the interpolated `Transform`; then `BurstAt`. |
| System | `particles_mark_spawned` | TickEnd, PerWorld, +900 (`run_if` pools) | Before `settle_spawned` removes `JustSpawned`: emitters teleported or spawned by a tick start their next stream at their new position instead of streaking along the jump (also after a restore, a frame-stage teleport, or frames their world was hidden). |
| System | `draw_particles` | Render, PerWorld, -800 (`run_if` pools) | Pushes quads into the App `RenderQueue`. World layers are culled against `CameraView::visible_world`, screen layers against the virtual screen. |
| Restore hook | `after_restore` | per world | Releases emitter pools whose owner lost its `ParticleRuntime` in a restore. |

## Behaviour notes

- A full pool overwrites its particles in turn (like the old engine), so a constant stream keeps
  its newest particles. `stats().replaced` counts the overwrites.
- `create()` refuses a pool that would go over the budget and counts it in `stats().rejected`. An
  emitter that can't get a pool retries every frame; one warning is logged per world.
- `EmitterConfig::burst` fires when an active emitter first gets its pool, and again whenever
  `active` goes from false to true. `pending_burst` is emitted and cleared even while the emitter
  is inactive.
- `mark_modified` on the config asset, or pointing the emitter at another asset, applies the new
  config in place. A capacity change moves the pool and keeps the particles that fit.
- Particles advance on `Time::frame_dt` (clamped to 1 s) in every visible world, so they keep
  moving while the App is paused.
