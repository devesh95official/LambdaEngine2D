# lam_physics

Pure module (no raylib).

Tile collision (`TileWorld`, `move_box` swept boxes), colliders (box, circle, convex polygon),
a counting-sort spatial hash, linear impulse bodies, layers and masks, triggers and contact
events, hazard tiles, platformer and top-down motors, moving platforms with riders, swept
projectiles, spatial queries, steering helpers and interactables, deterministic and
allocation-free in steady state.

## Usage

```cpp
App app({.title = "game"}, argc, argv);
DefaultPlugins plugins;                          // includes PhysicsPlugin
plugins.physics.gravity = {0, 600};              // Dynamic bodies only; motors carry their own
app.add_plugins(plugins);
World &w = app.main_world();                     // or ctx.world() in a startup system
const Entity hero = w.create();                  // feet at Transform.position, 10 x 14 body above
w.add<Transform>(hero, Transform{{48, 224}});
w.add<Collider>(hero, Collider{.offset = {0, -7}, .half = {5, 7}});
w.add<PlatformerMotor>(hero);                    // adds Velocity; write move_x/jump_* every tick
for (const CollisionStarted &c : ctx.read<CollisionStarted>()) hit(other(c, hero));   // PostPhysics
```

`examples/basics/08_physics` is the full version (tile world, falling crates and balls, a lift, a
trigger); `--headless --frames N` prints what happened and the state hash.

## Components (all `S N H`: saved, snapshotted, hashed; registered in `build()`)

| Component | Header | Written by | Notes |
|---|---|---|---|
| `Velocity` | `components.h` | game, motors, path followers, solver | px/s. Every mover needs one; RigidBody and the motors add it (on_add hook) |
| `Collider` | `components.h` | game | Box / Circle / Polygon (`PolygonShapes` id); `layer`, `mask`, `trigger`, `one_way` |
| `RigidBody` | `components.h` | game; solver wakes `sleeping` | linear only; Dynamic gets `PhysicsConfig::gravity`, damping, impulses |
| `Projectile` | `components.h` | game; `pierce` spent by physics | swept circle; stops at its last hit with zero Velocity, never destroyed by physics |
| `Interactable` | `components.h` | game | for `find_interactable` |
| `PlatformerMotor` | `motors.h` | game (intent), Physics -1500 (state) | coyote, buffer, jump cut, slopes + snap, one-way + drop, ladders |
| `TopDownMotor` | `motors.h` | game (intent), Physics -1500 (state) | corner assist, walled step |
| `MovingPlatform` | `motors.h` | Physics -1800 (`last_delta`) | carries `PlatformerMotor` riders at -1600 |
| `PathFollower` | `motors.h` | Physics -1800 | moves Transform, writes Velocity |

Body type: a `RigidBody` decides; a `Collider` without one is Kinematic if it has `Velocity` or a
mover component, else Static (`components.h`).

## Resources

| Resource | Scope | Inserted by | Written in |
|---|---|---|---|
| `PhysicsConfig` | App | `PhysicsPlugin::build` (an existing one is kept) | game (gravity, iterations, pair capacity may change at run time) |
| `PhysicsWorld` (private) | World | `init_world` | Physics; rebuilt by the restore hook (not snapshotted: a cache) |
| `PolygonShapes` | World | `init_world` | game (`add`); ids are stable |
| `TileWorld` | World | `init_world` (or `ensure_tile_world`) | tilemap (FrameBegin -1000, `set_tile`) or the game by hand |
| `PhysicsStats` | World | `init_world` | Physics -1200 / -1100 (diagnostics, not hashed) |
| `PhysicsContactSet` (private) | World | `init_world` (`snapshot_resource`, kSnapshot + kSerialize, not hashed) | Physics -1000: the previous-contact set, saved state |

## Events (World scope)

| Event | Emitted | Read |
|---|---|---|
| `CollisionStarted`, `CollisionEnded` | Physics -1000 (previous-contact diff) | PostPhysics, same tick |
| `TriggerEntered`, `TriggerExited` | Physics -1000 | PostPhysics, same tick |
| `TileHazardTouched` | Physics -1000, every tick while touching | PostPhysics, same tick |
| `ProjectileHit` | Physics -1300 | PostPhysics, same tick |
| `Interacted` | game (Simulate) | PostPhysics, next Simulate |

Use `other(event, self)` and `normal_from(event, self)` instead of swapping `a`/`b`.

## Systems (all PerWorld)

| Stage | Order | System | Does |
|---|---|---|---|
| Physics | -1800 | `path_followers`, `moving_platforms` | move kinematic platforms; `MovingPlatform::last_delta` |
| Physics | -1700 | `static_index` | rebuild the static grid only when static colliders changed |
| Physics | -1600 | `carry_riders` | move riders by their platform's `last_delta` |
| Physics | -1500 | `motors` | platformer and top-down motors against tiles and entity solids |
| Physics | -1400 | `integrate` | Velocity; Dynamic gravity, damping and the tile sweep |
| Physics | -1300 | `projectiles` | swept circles; `ProjectileHit` |
| Physics | -1200 | `broadphase` | counting-sorted dynamic grid; pairs in (a.index, b.index) order |
| Physics | -1100 | `narrowphase_solve` | exact shapes; sequential impulses, position projection |
| Physics | -1000 | `contact_events` | Started/Ended, Entered/Exited, `TileHazardTouched` |
| RenderUI | +1500 | `draw_colliders` | outlines into `DebugDraw` when `DebugOverlay::show_colliders` (F4) |

The previous-contact set is saved state (`PhysicsContactSet`, in snapshots and saves), so the
first tick after an undo or a load fires exactly the contact events the continuous run fired,
including contacts that began or ended after the last narrowphase (a body spawned, destroyed or
moved later in that tick or between ticks). The restore hook (`World::add_restore_hook`) rebuilds
both grids from the restored Transforms, so queries continue bit-identically; only a
snapshot without the set (a save written before it existed) has the set recomputed from the
restored state.

## Free functions

- Tiles (`tile_world.h`): `move_box`, `box_blocked`, `nearest_free_spot`, `tile_ray_clear`,
  `tile_raycast`. Pure; any stage.
- Queries (`queries.h`): `raycast`, `sweep_circle` (plus `RayQuery` overloads), `overlap_box`,
  `overlap_circle`, `query_neighbors`, `separation` (world, masked and pure forms),
  `find_interactable`, `move_body`, `refresh_queries`, `ensure_tile_world`. They test the current
  Transforms; see the note at the top of the header.
- Steering (`steering.h`): `seek`, `flee`, `hold_ring`, `pick_wander_goal` (two forms),
  `box_path_clear`, `can_see`, `sight_point`. Pure, never allocate.
- `step_velocity` (`motors.h`): the shared acceleration step.

## Tests and bench

`tests/physics` (`lam_physics_tests`, label `physics`): the old engine's collision and
movement tests (ported), platformer sweeps, bodies and solver, motors, broadphase, queries, projectiles,
steering, behaviour the example games rely on, determinism (`state_hash` across save/restore) and
`CHECK_NO_ALLOC` steady state. `lam_physics_bench --bodies 10000 --ticks 600` (label `bench`,
Release): uniform and clustered scenes, ms per tick, `PhysicsStats`, allocations per tick.
