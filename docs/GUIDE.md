# LambdaEngine2D runtime guide

How a frame runs, where simulation ends and presentation begins, how events travel, and the rules
that keep a run deterministic. [`RECIPES.md`](RECIPES.md) has a game skeleton and recipes. Each
`src/<m>/README.md` lists that module's systems, and `--print-schedule` prints the live system
order of any game.

---

## 1. The frame

Every system is a plain function `void fn(lam::Ctx &)` registered in one of twelve stages. Seven
run once per fixed tick, the rest once per frame.

```
App::frame()
  FrameBegin   Once: platform -10000, raw input -5000, frame input -4000, assets -3000, scene apply -2000
               PerWorld: tilemap_sync -1000                       (commands applied per world)
  repeat n times   (n = 0..max_ticks_per_frame; 0 while paused or while startup assets load)
    TickBegin Once: scene ops -2000, input publish -1500, InputDriver -1400, recorder -1300
    for each world that ticks (bottom of the world stack first):
      TickBegin -> Input -> Simulate -> Physics -> PostPhysics -> Animate -> TickEnd
      (TickBegin PerWorld: copy_prev_transforms -2000, tilemap_sync_tick -1900)
      (that world's commands are applied after every stage)
    TickEnd Once: --hash-every logger +1000
  for each visible world:  Update -> Render -> RenderUI    (Once systems: order < 0 before, >= 0 after)
  FrameEnd     Once: render plan -1000, submit -500, clear +9000, swap +10000
```

| Stage | Runs | Game code does here | Engine does here (order) |
|---|---|---|---|
| FrameBegin | per frame | rarely anything | platform, input polling, assets, scene apply, tilemap sync |
| TickBegin | per tick | rarely anything | scene ops, input publish, bots, recording; per world `copy_prev_transforms` -2000, `tilemap_sync_tick` -1900 |
| Input | per tick | read `input_frame(ctx)`, write intent (motor fields, game commands) | none |
| Simulate | per tick | rules, AI, spawning, turn loop | grid index -1500, path requests -1100 |
| Physics | per tick | usually nothing | parents -1900, path followers and platforms -1800, static index -1700, riders -1600, motors -1500, integrate -1400, projectiles -1300, broadphase -1200, solve -1100, contacts -1000 |
| PostPhysics | per tick | react to this tick's contact events; choose `Animator` clips, params and facings | none |
| Animate | per tick | rarely needed: runs after the engine animators, so a clip chosen here shows a tick late; overwrite what the animators wrote (`kFlipX`, region) at order >= 0 | motor bridge -1500, animators -1400, tweens -1300, grid movers -1200, animated tiles -1100 |
| TickEnd | per tick | end-of-tick bookkeeping; scene pushes caused by game events | parents -1000, lifetimes -900, settle spawned +1000, align pools +1100 |
| Update | per frame | presentation: HUD text, UI clicks into intents | frame tweens -1500, fades -1400, camera follow/shake/view -1300..-1100, particles -1000, audio -900, UI -800 |
| Render | per frame | custom world-space drawing into `RenderQueue` | tiles -1000, sprites -900, particles -800, lights -700 |
| RenderUI | per frame | custom screen-space drawing | UI -1000, colliders +1500, debug draw +2000; Once: toasts +500, scene fade +1000, overlay +2000 |
| FrameEnd | per frame | nothing (Once only) | render plan and submit, swap |

### Orders

A system's place in a stage is `(order, registration index)`. Game systems use orders in
(-1000, 1000), so they run after the engine's negative orders and before its positive ones: a game
Physics system at order 0 runs after `contact_events` (-1000), and game Animate systems run after
every engine Animate system. Pick the stage by when the result has to show:

- Animation choices go in Simulate or PostPhysics. `motor_anim_bridge` (-1500) and `animators`
  (-1400) run before any game Animate system and apply `Animator::clip`, `AnimParams` and
  `Animator::facing` as they are at that point, so a clip chosen in a game Animate system shows
  on the next tick.
- Overrides of animator output go in Animate at order >= 0, or in Update. The animators write
  `Sprite::region` and `kFlipX` every tick, so a flip written in Simulate is lost. On an animated
  entity, mirror through `Animator::facing` instead.
- Contact reactions go in PostPhysics, which reads contact events (Physics -1000) and
  `ProjectileHit` (-1300) in the same tick.
- Spatial queries work in any tick stage. `overlap_box`, `raycast`, `separation` and the rest test
  each candidate at its current `Transform`, and the first query of a tick rebuilds their grids,
  so Input/Simulate queries see this tick's state and replay identically after undo or load. If
  you move bodies by writing `Transform` after a query in the same tick, call `refresh_queries(w)`.
  The pure `TileWorld` tests (`can_see`, `tile_ray_clear`, `box_blocked`) need no physics.

### Scopes, commands, startup, time

`SystemScope::PerWorld` (the default) runs once for each world taking part in the stage, with
`ctx.world()` set to that world. `SystemScope::Once` runs once per frame (or tick) with
`ctx.world()` = the focused world, or the main world if none is focused. Input, Simulate,
Physics, PostPhysics and Animate allow PerWorld only; FrameEnd allows Once only. `add_system`
checks this.

`ctx.commands()` records structural changes (spawn, add, remove, destroy, teleport). The App
applies them after every PerWorld stage run on that world, and after each Once pass for the
focused world. Records whose entity died in the meantime are dropped.

`App::add_startup(fn, name)` runs `fn` once on the main world before frame 1 (Once, FrameBegin,
`ctx.rng` allowed, root asset scope current).

`ctx.time()` is per world. `tick` counts this world's ticks, `app_tick` the App's fixed steps (the
replay key), `tick_dt` converts ticks to seconds, `frame_dt` is this frame's real seconds
(`tick_dt` under `--step`; measure performance with `real_seconds` deltas), and `alpha` in [0, 1]
is the interpolation factor. Render draws `lerp(PrevTransform, Transform, alpha)`; a world that
does not tick this frame has `alpha = 1`. Spawned and teleported entities appear in place instead
of gliding in (`JustSpawned`).

---

## 2. Worlds and scenes

- The App owns every `World`. `App::main_world()` exists from construction; `create_world` makes
  more. Each plugin's `init_world` inserts its World resources (`TileWorld`, `CameraView`,
  `UiTree`, `ParticlePools`, ...) into every world.
- The world stack (bottom first, at most 8) says which worlds take part in a frame. Each entry has
  `ticks` (runs tick stages), `visible` (runs Update/Render/RenderUI) and `input` (the topmost
  input world is focused; `input_frame(ctx)` is neutral everywhere else).
- A `Scene` owns a World and an asset scope. `SceneStack` creates the world before `enter`,
  destroys it after `exit`, and derives the world stack from the scenes: `overlay()` keeps lower
  scenes visible, `blocks_ticks()` stops them ticking (they render their last state) and
  `blocks_input()` takes focus from them.
- Scene operations are queued FIFO and applied at FrameBegin -2000 and at every tick boundary
  (TickBegin -2000). A second transition waits for the first.

The first `push` takes the main world out of the frame. Until a scene is pushed, the App's own
stack is used (`{main_world}` by default). While the scene stack holds a scene, the world stack
comes from the scenes only, so the main world neither ticks, renders nor gets input until the
stack is empty again. Startup systems still run on it. A game that pushes menus or overlays must
run its gameplay in a `Scene`: a `MenuScene` overlay pushed over gameplay in the main world hides
that gameplay instead of covering it.

Plugin systems run on every world in the stack. PerWorld systems added with `App::add_system` also
run on menu, confirm and overlay worlds, which tick while nothing blocks them, and
`ctx.local<GameState>()` aborts there. Gate every game system with
`.run_if = &has_local<GameState>` (a World resource only your game world holds), or register it
for one world (`SystemDesc::only_world`, `Scene::add_system`). System names are unique per
(stage, `only_world`), so a re-created scene world may reuse its names.

`run_if` skips one system call; it doesn't stop the world. Its engine systems keep going: motors
keep applying their last intent (physics never clears it), `integrate` keeps moving bodies by
`Velocity`, projectiles keep flying, animators, tweens and grid movers keep advancing, path
requests are still served and `Lifetime` keeps counting. To freeze a world (pause, game over),
stop its ticks: push an overlay scene whose `blocks_ticks()` is true (every engine `MenuScene`
does), or without scenes set its `WorldEntry` `ticks = false` in `App::set_world_stack`.
`App::set_paused(true)` holds the ticks of every world while frame stages still run. To stop only
the bodies, zero the motors' intent fields and `Velocity`.

App resources (one per App) are reached with `ctx.res<T>()`, World resources (one per world) with
`ctx.local<T>()`. Using the wrong one aborts with a message naming the right call. App resources
include `SettingsStore`, `AssetServer`, `ActionMap`, `SceneStack`, `PrefabLibrary`, `Audio`,
`RenderQueue` and `ToastQueue`; World resources include `TileWorld`, `CameraView`, `Lighting`,
`UiTree`, `ParticlePools`, `GridIndex`, `PathService`, `TurnScheduler` and `FovMemory`.

---

## 3. Simulation and presentation

| | Simulation (rules) | Presentation (view) |
|---|---|---|
| Stages | Input, Simulate, Physics, PostPhysics, Animate, TickEnd; startup; `Scene::enter` | Update, Render, RenderUI |
| Time base | integer ticks (`Time::tick_dt` only to convert) | real seconds (`Time::frame_dt`) |
| Randomness | `ctx.rng("stream"_id)`: per world, snapshotted, hashed | `ctx.fx_rng()`: App-level, never hashed |
| State | reflected components with `kDefaultFlags`, World resources registered with `snapshot_resource` | `Sprite`, `RenderOffset`, `Camera2D`/`CameraFollow`/`CameraShake`, `FrameTween`, `SpriteFade`, `ParticleEmitter`, `AudioEmitter`, unreflected runtime data |
| Saved / undone / hashed | saved, snapshotted, hashed | saved and snapshotted (`kSerialize \| kSnapshot`), never hashed; runtime data is neither |
| Talks to the other side | emits events: `PlaySound`, `BurstAt`, `ShakeAt`, game events | reads those events; sends decisions back with `InputSystem::queue_intent` |
| Engine parts | physics and motors, `Animator`, `Tween`, `GridMover`, `TurnScheduler`, `Lifetime` | cameras, particles, audio, UI, `FrameTween`, fades, lights |

- Animation is simulation: `Animator` and `Tween` advance on tick time and are hashed, so choose
  clips in tick stages (Simulate or PostPhysics). Visual-only touches (tint blink, `RenderOffset`,
  `kFlipX` on a sprite without an `Animator`) belong in Update.
- `lam_add_game(... SIM_LIBRARY <game>_sim SIM_DEPS <pure targets>)` puts the rules in a library
  that links only pure targets (no raylib), so rules run in tests and `--headless`. The executable
  links `lam_engine` and holds scenes, camera, HUD and effect assets.

### Positions: feet or centre

Passing the wrong point is the most common silent error. `Transform.position` is the entity's
anchor; most APIs take it, some take a centre or an absolute box.

| API | Point or box it takes |
|---|---|
| `Transform.position` of a sprite or motor body | the anchor. With the default `Sprite::pivot` (0.5, 1) it is the **feet** (bottom centre of the frame) |
| `Collider::offset` | collider centre relative to `Transform.position`. A feet-anchored body uses `{0, -half.y}` |
| `Sprite::pivot` | fraction of the frame size; values outside 0..1 are fine (a canopy above the feet) |
| `collider_box(c, pos)`, `box_blocked`, `move_box`, `nearest_free_spot` | absolute world rects. `nearest_free_spot` returns the free box's **top-left**: `position = *spot + (position - box.min())` |
| Tiled and native map objects (`Tilemap` spawning) | a rectangle (or a 1 x 1 native object) spawns its prefab at the **rect centre**, a point object at the point, and an object with the custom property `"anchor": "bottom"` at the rect's **bottom centre**. Use a point at the feet or `"anchor": "bottom"` for feet-anchored prefabs |
| `GridPosition` entities (`try_step`, `grid_movers`) | the **cell centre** (`TileWorld::tile_center`). Pick a `Sprite::pivot` that puts the feet there |
| `serve_path_requests`, `follow_route(route, tiles, pos)` | the search starts from the `GridPosition` cell, else the cell under `Transform.position + PathRequest::start_offset`. `follow_route` compares `pos` with waypoint **cell centres**: pass that same point. A feet-anchored walker passes its `Collider::offset` as `start_offset` and `position + offset` to `follow_route`, so its centre runs through cell centres |
| `FlowField::direction`, `TileWorld::tile_at`, `BurstAt::position`, `PlaySound::position`, `raycast` origin | absolute world px |
| steering `flee`, `box_path_clear`, `pick_wander_goal` | `box` is the body's collision box **relative to the position arguments**, `collider_box(collider, {0, 0})`; the positions are anchors (feet). `pick_wander_goal` writes an anchor: where `Transform.position` should go |
| `sight_point(body)`, `can_see` | `sight_point` takes the **absolute** body box and returns its eye, one pixel above the bottom centre; `can_see` measures eye to eye |
| `separation`, `query_neighbors` | collider centres (`Transform.position + Collider::offset`) |
| `CameraFollow::target` | the target's interpolated `Transform.position` (the feet) plus `CameraFollow::offset` (`{0, -8}` aims 8 px higher); following an invisible child entity whose `Parent::local` is the offset also works |
| `Light2D`, `Text` | the interpolated `Transform.position` plus their own `offset` field (world px) |
| `ParticleEmitter` | the interpolated `Transform.position`; the config's `spawn_area` is around it |
| `FadeOccluder::top` | relative to the occluder's Transform (its feet); revealers compare their feet |
| `current_slice` | relative to the sprite pivot of the untrimmed frame |
| `UiNode::rect` | virtual pixels from the parent's anchor point |
| `InputFrame::pointer_world` | world px through the focused world's camera |

---

## 4. Events

Register each event type once: `app.add_event<T>(EventScope::World, capacity)`. World scope is the
default; App scope is only used for `AssetEvent<T>`, `WindowResized` and `LifecycleChanged`.
Registering again with the same scope does nothing, and the first call fixes the capacity
(`App::reserve_event<T>` raises it). Emitting or reading an unregistered type aborts.

Engine events are registered by the plugin that owns them (`PlaySound` by audio, `BurstAt` by
particles, `ShakeAt` by render). Rules that emit them must also run without that plugin (headless
tests, a sim library), so the rules plugin registers them itself in `build()`, e.g.
`app.add_event<PlaySound>()`. For an optional plugin's event, `ctx.try_emit(e)` emits only if the
type is registered, and `ctx.has_event<T>()` says whether it is.

`ctx.emit(T{...})` sends a World event to `ctx.world()`'s channel; entity-bearing events never
cross worlds. `ctx.read<T>()` is a span of the events emitted since this stage last ran on this
world, in emission order, valid until the system returns.

### The read window

A reader in stage S sees an event exactly once, at most one tick late: everything emitted after the
start of its previous run and before the start of its current run. Events emitted inside S itself
(by an earlier system in the same pass) reach S's readers on the next run.

| Emitted in | Same tick (or frame) | Next tick |
|---|---|---|
| Input | Simulate, Physics, PostPhysics, Animate, TickEnd; Update this frame | Input |
| Simulate | Physics, PostPhysics, Animate, TickEnd; Update this frame | Input, Simulate |
| Physics (contacts -1000, `ProjectileHit` -1300) | PostPhysics, Animate, TickEnd; Update this frame | Input, Simulate, Physics |
| PostPhysics | Animate, TickEnd; Update this frame | Input, Simulate, PostPhysics |
| Animate (`AnimationEvent`, `TweenFinished`) | TickEnd; Update this frame | Input, Simulate, Animate |
| Update (`UiEvent`) | Render, RenderUI this frame | Update next frame; tick stages at the next tick (see rule 7) |

A frame that runs zero ticks loses nothing: tick readers get the events at the next tick, even if
it runs in a later frame. Events emitted into a world while it does not tick are not delivered to
its tick stages.

### Engine events

World scope unless noted.

| Event | Emitted | Typical reader |
|---|---|---|
| `CollisionStarted`, `CollisionEnded`, `TriggerEntered`, `TriggerExited`, `TileHazardTouched` | Physics -1000 | PostPhysics, same tick |
| `ProjectileHit` | Physics -1300 | PostPhysics, same tick |
| `Interacted` | game code (Simulate) | PostPhysics, next Simulate |
| `AnimationEvent` | Animate -1400 | Simulate next tick (hitboxes), Update |
| `TweenFinished` | Animate -1300 | Simulate next tick |
| `WorldRestored` | `World::restore`, `deserialize_world` | Simulate, Update |
| `PlaySound` | any tick stage | `audio_world`, Update -900 |
| `BurstAt` | any tick stage | `particles_update`, Update -1000 |
| `ShakeAt` | any tick stage | `camera_shake`, Update -1200 (adds trauma to the world's `CameraShake`s) |
| `UiEvent` | Update -800 | frame-stage game code only |
| `AssetEvent<T>` (App) | FrameBegin -3000 | frame stages |
| `WindowResized`, `LifecycleChanged` (App) | FrameBegin -10000 | frame stages |

`CollisionStarted` orders its pair by entity index (`a.index < b.index`) and its normal points from
`a` to `b`. Read it from your entity's side with `other(c, self)` and `normal_from(c, self)`
(`physics/events.h`; `other` also takes a `CollisionEnded`) rather than swapping by hand. Each
touch reports one Started and one Ended however long it lasts (shapes within 1/32 px count as
touching), and a restore (undo, load) recomputes the contact set, so nothing fires that did not
change.

Two colliders interact only when each one's `layer` is in the other's `mask`. An interacting pair
reports contacts whatever the body types (two motor bodies that don't block each other still report
`CollisionStarted`); only Static-Static pairs never do. The pair and blocking rules are in
`physics/components.h`. `TileHazardTouched` has no started/ended pair: it repeats every tick while
a body touches a hazard tile. A `ProjectileHit` that spends the last `pierce` (or hits a blocking
tile) leaves the projectile resting at the contact with zero `Velocity`; physics never destroys it.

---

## 5. Determinism rules

The same binary, platform, seed and input stream give the same `World::state_hash()`, including
across a save and load, as long as game code follows these rules.

1. Change simulation state only in tick stages, startup systems and `Scene::enter`.
2. Count time in ticks. Timers, cooldowns and durations are integers (`std::uint32_t ticks`).
   Never accumulate `frame_dt` (or any float time) in rules; `tick_dt` converts ticks to seconds
   for physics maths only.
3. Draw randomness from `ctx.rng("stream"_id)` in tick stages, one stream per independent rule.
   Presentation uses `ctx.fx_rng()`. Never `std::rand`, `<random>` devices, wall-clock time or
   pointer values.
4. Tick systems never call `Audio` or `ParticlePools` directly; they emit `PlaySound`/`BurstAt`.
   Camera shake works the same way: emit `ShakeAt{.trauma}` and `camera_shake` (Update -1200) adds
   it to the world's `CameraShake`s.
5. UI never changes simulation state directly. Frame code calls `InputSystem::queue_intent(Intent)`;
   the intent arrives in exactly one later tick's `InputFrame::intent_list()` and is recorded with
   it, so replays reproduce it. Only the focused world sees it. A rule that cannot act on it in
   that tick (a turn-based game between turns) copies it into reflected state, such as a pending
   field of a snapshot resource, or it is lost. An `InputDriver` (bot) gets each tick's frames
   already filled with live input and the queued intents: it edits them in place
   (`clear_devices()`, then `press`, `hold`, `set_axis`) and never assigns `InputFrame{}`, which
   would drop menu selections.
6. Frame-stage writers touch only presentation state (`Sprite`, `RenderOffset`, camera components,
   `FrameTween`, your own unhashed resources), never `Transform`, `Velocity` or game components.
7. Tick systems must not let frame-emitted events (`UiEvent`) change simulation state. Route them
   through intents (rule 5).
8. Issue gameplay scene operations from tick systems. They then apply at the next tick boundary,
   which replays. The engine menu scenes already turn selections into intents.
9. Keep rule state reflected. Components with `LAM_REFLECT` (default flags) and World resources
   registered with `World::snapshot_resource<T>()` are saved, undone and hashed. Unreflected
   components are runtime data: pruned on restore, never saved.
10. Rebuild derived state in a restore hook. If your rules fill a cache or the `TileWorld` from
    other state, rebuild it in `World::add_restore_hook`, which runs after undo and load.
11. `lam_add_game` builds game code with `-ffp-contract=off` and no fast-math. Use the engine maths
    (`det_sin`, `det_cos`, `det_atan2`, `angle_of`, `from_angle`, `Rng` helpers) in rules, not
    `std::sin`.
12. Iteration order is deterministic (dense pool order). Don't key rule behaviour on unordered
    containers of pointers.
13. `--headless`, `--step`, `--record-input` and `--replay` load assets synchronously and turn hot
    reload off, so asset completion depends on the tick count, not IO. A scene's world does not
    tick until the loads of its own asset scope (and their dependencies) are Ready or Failed
    (`Transition::wait_for_assets`; unrelated loads elsewhere don't hold it), and the main world
    waits for startup assets (`AppConfig::wait_for_startup_assets`).
14. Replays are per build. A replay records `LAM_BUILD_ID`, the OS and the tick rate, and another
    build refuses it unless `--replay-any-build` is given. Bit-equality across platforms is not
    promised.

---

## 6. Lifetimes

| Thing | Valid until | Store instead |
|---|---|---|
| `World::get<T>` / `try_get<T>` reference or pointer | the next structural change to pool `T` (add/remove of `T`, destroy, restore) | `Entity` |
| A `View` in use | its pools are locked: structural changes to them abort in every build | record changes with `ctx.commands()` |
| `AssetServer::get<T>` pointer | the end of the current stage | `AssetId<T>` |
| `UiTree::get(UiId)` pointer | the next `add`/`remove` on that tree | `UiId` |
| `Catalog<T>::find` pointer | the next `add` | the index (`index_of`) |
| `ctx.read<T>()` span, `Ctx` itself | the end of the system call | copy what you need |
| Resource references (`res`, `local`) | `remove<T>()` or the owner's destruction (resources never move) | the reference is fine |
| A scene's `World&` | `exit` of that scene | look it up again |
| `Entity` handles | never dangle: a stale handle is just not `alive()` | check `alive` or `try_get` |

`World::restore`, `History::undo/redo` and `deserialize_world` also check that no view is iterating.

---

## 7. Assets

- Components store `AssetId<T>`, never pointers. `AssetServer::load<T>(path)` returns at once and
  retains the asset in the current scope; `get<T>(id)` is null until it is Ready.
- Plugin `build` and startup systems use the root scope, which lives until shutdown. During
  `Scene::enter` the scene's scope is current; it is released after the scene exits.
- Paths in code (`load<T>`), in prefabs and in saves are relative to the asset root
  (`AssetsConfig::root`, then `LAM_ASSET_ROOT`, then the source folder in dev builds via
  `<exe>.assetroot`, then `<exe_dir>/assets`). Paths written inside an asset file are relative to
  that file, with `.` and `..` normalised: the `.anim.json` `sheet`, the `.sounds.json` `files`,
  the `.tsj` `image`, the `.tmj` tileset `source` and Aseprite `meta.image`. So
  `sprites/hero.anim.json` names its sheet `"hero.json"`; `"sprites/hero.json"` would resolve to
  `sprites/sprites/hero.json`.
- Engine formats (`.anim.json`, `.sounds.json`, `.map.json`, `.tileset.json`, `.theme.json`,
  `.particles.json`, prefabs) are strict: unknown keys are errors. A failed dependency fails its
  parent (a missing sheet fails the graph, a missing sound file fails the bank), and the component
  that names the failed asset never gets it. Check `AssetServer::state(id)` and `error(id)` when
  something doesn't appear, and copy keys from the module README examples instead of guessing.
- Runtime-made assets: `AssetServer::add<T>(value, name)` (e.g. particle configs built in code). A
  non-empty `name` becomes the asset's path, so prefab and save fields that name it resolve to it
  while it lives; add it before `PrefabLibrary::load` runs. An unnamed runtime asset saves as `""`
  and loads back as none. Atlas pages are named this way (`"@atlas/<group>/<n>"`, packed in a
  deterministic order), so a saved `Sprite::region` on an atlas page loads back while the same
  assets are loaded.
- After editing a Ready asset through `get_mut`, call `mark_modified(id)` (runtime assets too). Its
  version changes and `AssetEvent<T>::Modified` is emitted, so consumers such as `tilemap_sync`
  and particle pools pick up the edit.

---

## 8. Inspecting a running game

| Tool | What it shows |
|---|---|
| `--print-schedule` | every system with stage, scope, order and plugin, then exits |
| `--hash-every N` | logs `hash tick=<n> world=<name> <16 hex>` for every ticking world each N app ticks (diff two runs, or a run and its replay) |
| `--record-input F` / `--replay F` | input recording and exact replay (same build) |
| F3 / F4 / F11 (dev builds) | debug overlay / collider overlay / fullscreen |
| `DebugOverlay::stat(label, value)`, `DebugOverlay::text(line)` | your own numbers on the F3 overlay |
| `DebugDraw` (World resource) | `line`/`rect`/`circle`/`text` drawn this frame at RenderUI +2000, cleared at FrameEnd |
| `World::state_hash()` | the hash itself, for a summary line or a test |

### Command-line flags

Engine flags are parsed into `LaunchOptions` by the `App` constructor. Unknown flags go to
`app.options().extra`; test them with `app.options().has("--bot")`.

| Flag | Effect |
|---|---|
| `--headless` | NullPlatform, no window/audio/backends, one tick per frame, synchronous assets |
| `--hidden` | a real but hidden window (GPU tests, screenshots). Frames aren't vsync-paced (on macOS a hidden window runs unthrottled while ticks follow real time), so `--frames N` covers far fewer than N ticks: add `--step` to reach a given tick, or check tick counts with `--hash-every` |
| `--step` | one tick per frame, and frame stages get `frame_dt = tick_dt`, so camera follow, `FrameTween`s, particles and fades advance one tick's worth per frame at any machine speed (`--step --hidden --frames N --screenshot` is reproducible) |
| `--frames N` | quit after N frames |
| `--seed S` | override `AppConfig::seed` (decimal or `0x` hex) |
| `--hash-every N` | log `hash tick=<n> world=<name> <16 hex>` for each ticking world every N ticks |
| `--record-input FILE` / `--replay FILE` | record every tick's input / replay it (same build; `--replay-any-build` to force) |
| `--screenshot FILE` | save the last frame |
| `--draw-every N` | render every Nth frame (frame end still swaps) |
| `--print-schedule` | log every system with stage, scope, order and plugin, then exit |
| `--no-vsync` | benchmarks |
