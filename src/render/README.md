# lam_render

Example: `examples/basics/07_render`.

Sprites and layers, cameras (follow the interpolated target, dead zone, look-ahead, bounds, shake,
pixel-perfect zoom), the virtual screen, the sort-keyed `RenderQueue` and `plan_batches`, load-time
atlases, fonts and text, shaders and materials, render targets and passes, and the optional 2D light
pass. The pure `lam_render` builds and plans every frame headless (tests read `RenderPlan`); the
raylib backend `lam_render_rl` uploads textures and submits the plan through its own batcher.

```cpp
lam::App app({.title = "Walkers", .virtual_size = {480, 270}}, argc, argv);
app.add_plugin<lam::PlatformPlugin>();                 // window (skip for --headless)
app.add_plugin<lam::AssetsPlugin>();
app.add_plugin<lam::RenderPlugin>(lam::RenderConfig{.clear_color = lam::Color{24, 28, 36, 255}});
app.add_plugin<lam::RaylibRenderPlugin>();             // GPU backend (skip for --headless)
lam::World &w = app.main_world();
lam::Entity hero = w.create();
w.add<lam::Transform>(hero, lam::Transform{{100, 100}});
w.add<lam::Sprite>(hero, lam::Sprite{.region = sheet_frame});   // y-sorted at kLayerWorld
lam::Entity cam = w.create();
w.add<lam::Camera2D>(cam);
w.add<lam::CameraFollow>(cam, lam::CameraFollow{.target = hero, .dead_zone = {-16, -24, 32, 48}});
```

Drawing that is not an entity goes straight into the queue from a Render/RenderUI system:
`ctx.res<RenderQueue>().rect(...)`, `.region(...)`, `draw_text(q, font, "hi", pos, style)`,
`q.triangle(a, b, c, color, layer)` (or `draw_triangle(q, ...)` from `shapes.h`). World layers
(< 200) are drawn through the world's camera; screen layers (>= 200) are virtual pixels in
painter's order. Simulation code shakes the camera with `ctx.emit(ShakeAt{.trauma = 0.4f})`
instead of touching `CameraShake`.

## Components

| Component | Header | Flags | Written in | Notes |
|---|---|---|---|---|
| `Sprite`, `SpriteFade`, `Shadow`, `FadeOccluder`, `FadeRevealer` | `render/sprite.h` | S N | Animate (region), Update | presentation |
| `RenderOffset` | `render/sprite.h` | N | Update (frame tweens) | presentation |
| `Text` | `render/font.h` | S N | any | label at Transform + offset |
| `Camera2D`, `CameraFollow`, `CameraBounds`, `CameraShake` | `render/camera.h` | S N | Update | frame time |
| `CameraFollowState` | `render/camera_math.h` | N | Update -1300 | engine-added follow state |
| `Light2D` | `render/lighting.h` | S N | any | drawn at Transform + `offset` when the world's `Lighting` is on |

## Resources

| Resource | Scope | Inserted by | Written in |
|---|---|---|---|
| `RenderQueue` | App | RenderPlugin | Render/RenderUI; cleared FrameEnd +9000 |
| `RenderStats` | App | RenderPlugin | FrameEnd -1000 (plan), -500 (backend: draw calls, timings) |
| `RenderPlan` (`render_plan.h`) | App | RenderPlugin | FrameEnd -1000: sorted batches + vertex staging |
| `ScreenCapture` (`render_plan.h`) | App | RenderPlugin | set `request`; filled by render_submit |
| `RenderBackendStats` (`render_plan.h`) | App | RaylibRenderPlugin | FrameBegin -2500, FrameEnd -500 |
| `VirtualLayout` | App | RenderPlugin | FrameBegin -9000 |
| `AtlasSet`, `Materials`, `RenderPasses`, `RenderDefaults`, `RenderConfig` | App | RenderPlugin | load time / build |
| `CameraView`, `Lighting` | World | RenderPlugin `init_world` | Update -1100 (view) |

## Events

| Event | Scope | Emitted by | Read by |
|---|---|---|---|
| `ShakeAt` (`render/camera_math.h`) | World, capacity 64 | game tick systems (any stage) | `camera_shake` (Update -1200): `trauma` added to every `CameraShake` of that world |
| `AssetEvent<Texture>`, `AssetEvent<Shader>` | App | assets | `texture_upload` (backend) |

## Systems

| Stage | Scope | Order | System | Target |
|---|---|---|---|---|
| FrameBegin | Once | -9000 | `virtual_layout_update` | lam_render |
| FrameBegin | Once | -2500 | `texture_upload` (new/changed `Texture`s, atlas pages) | lam_render_rl |
| FrameBegin | Once | -2400 | `atlas_shape_page` (`RenderQueue::set_shape_texture`) | lam_render |
| Update | PerWorld | -1400 | `fade_occluders`, then `sprite_fades` | lam_render |
| Update | PerWorld | -1300 | `camera_follow` (interpolated target) | lam_render |
| Update | PerWorld | -1200 | `camera_shake` (`ShakeAt` events, trauma decay) | lam_render |
| Update | PerWorld | -1100 | `camera_view` (World `CameraView`) | lam_render |
| Render | PerWorld | -2000 | `begin_view` | lam_render |
| Render | PerWorld | -900 | `collect_sprites` (interpolated, culled; `RenderOffset`, `Shadow`, `Text`) | lam_render |
| Render | PerWorld | -700 | `draw_lights` (run_if `Lighting::enabled`) | lam_render |
| RenderUI | PerWorld | -2000 | `resume_view` | lam_render |
| RenderUI | PerWorld | +2000 | `draw_debug` (World `DebugDraw`; LAM_DEV) | lam_render |
| RenderUI | Once | +2000 | `draw_overlay` (`DebugOverlay`, `Profiler`; F3; LAM_DEV) | lam_render |
| FrameEnd | Once | -1000 | `render_plan` (sort, `plan_batches`, `RenderStats`) | lam_render |
| FrameEnd | Once | -500 | `render_submit` (upload, draw, present, capture, `--screenshot`) | lam_render_rl |
| FrameEnd | Once | +9000 | `render_clear` (`RenderQueue`, `DebugDraw`s) | lam_render |
| FrameEnd | Once | +10001 | `render_swap_timer` (`RenderStats::gpu_swap_ms`) | lam_render_rl |

`RenderPlugin` also registers `align_pools<Transform, PrevTransform, Sprite>` (TickEnd +1100).

## Notes

### Cameras

`CameraFollow`, `CameraBounds` and `CameraShake` go on the camera entity; bounds on the target do
nothing. Follow aims at the target's interpolated `Transform.position` (the feet with the default
pivot). A target without a `Transform` leaves the camera still and the first frame with one snaps,
so it's fine to set `target` before the map has spawned the player. To aim above the feet set
`CameraFollow::offset` (`{0, -8}`), or follow a child anchor entity
(`set_parent(w, anchor, hero, Transform{{0, -8}})`). A world without `Camera2D` (HUD and menu
worlds) gets the default view where world px = virtual px.

### Sprites and text

- Pivots are fractions of the untrimmed frame and can go outside 0..1. A canopy whose bottom is
  14 px above the feet of a 32 px frame uses `pivot.y = 1 + 14 / 32`. Culling takes this into
  account.
- `FadeOccluder::top` is relative to the feet, and a revealer counts as behind when its feet are
  above the occluder's. See `fade.h`.
- `TextStyle::size` is in pixels: scale = size / `Font::base_size`. The built-in 6x8 font has base
  8, so 8 is 1x and 16 is 2x; 0 means the base size. Pixel fonts stay crisp at integer multiples.
- `Sprite::region.texture` is saved as its asset path. Atlas pages are named `@atlas/<group>/<n>`
  and resolve after a load as long as the same atlases were packed first. Other runtime textures
  have no path and load as slot 0, so re-resolve them or drive the sprite with an Animator. Undo
  snapshots copy ids exactly.

### Lighting

`Lighting::enabled` (World resource) turns it on, and the light target is created on first use.
`RenderConfig::lighting` only creates it up front. `Light2D::offset` moves a light off its entity
(not rotated); a child entity works too.

### Sorting and batching

- Sort key: `view | layer | depth | material | texture | clip` plus a 32-bit tiebreak. ECS sprites
  tie-break on entity index + 1 so sprites with equal feet never flicker. Screen layers keep push
  order.
- Batches break on view, screen/world space, target, clip, material, texture and 16384 quads, but
  not on layer. Shapes, shadows, lights and the built-in font live on the first atlas page, so a
  typical scene is a handful of draw calls (the bench scene: 4 at 10k sprites, 7 at 50k).
- Textures that aren't uploaded yet are skipped and counted in `RenderStats::skipped_unloaded`
  rather than drawn white.
- No per-frame allocation in steady state: the queue, plan, staging and VBO ring are all reused.
  macOS's OpenGL allocates inside GL calls, so `RenderBackendStats::driver_allocations` counts that
  separately.
- `RaylibRenderConfig::rlgl_batch` or `LAM_RENDER_RLGL_BATCH=1` draws through rlgl's own batch
  instead of the VBO ring. That path has no custom material shaders and no sharp upscale.

### Tests

`lam_render_tests` (headless), `lam_render_gpu_tests` (label gpu, hidden window, golden image
`tests/render/golden/gpu_scene.txt`; `LAM_UPDATE_GOLDEN=1` rewrites it), `lam_render_cpu_bench`
and `lam_render_bench` (label bench, `-DLAM_BUILD_BENCH=ON`).
