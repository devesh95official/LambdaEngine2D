# lam_tilemap

Pure module: links `lam_render` and `lam_physics`, no raylib.

Tile maps and the grid toolkit built on them: Tiled `.tmj`/`.tsj` and native `.map.json`/`.tileset.json`
(and ASCII rows) loaders, the per-world `TilemapInstance`, `TileWorld` filling, culled tile drawing with
animated tiles and fog, autotiling, budgeted A*, Dijkstra maps and flow fields, symmetric FOV with
explored memory, grid-step movement with an occupancy index, path requests and procgen.

## Usage

```cpp
app.add_plugins(DefaultPlugins{});                                   // adds TilemapPlugin
// startup: a map entity; tilemap_sync copies the asset into a TilemapInstance, fills TileWorld and
// spawns the map's objects whose type names a prefab.
Entity map = w.create();
w.add<Transform>(map);                                               // map origin
w.add<Tilemap>(map, Tilemap{.map = assets.load<TilemapAsset>("maps/level1.tmj")});
// a tick system: ask for a route (served at Simulate -1100), walk it, look around.
w.add<PathRequest>(hero, PathRequest{.goal = {20, 7}, .start_offset = col.offset});
motor.move = follow_route(w.get<PathRoute>(hero), ctx.local<TileWorld>(), t.position + col.offset);
update_fov(ctx.local<FovMemory>(), ctx.local<TileWorld>(), hero_cell, 8);   // + FogStyle -> fog
set_tile(w, map, str_id("ground"), {3, 4}, 0);                        // instance + TileWorld cell
```

See `examples/basics/10_tilemap` for a complete program (A* walk on a Tiled map, FOV and fog
toggle).

## Components, resources, events

| Name | Kind | Header | Flags / scope | Written by |
|---|---|---|---|---|
| `Tilemap` | component | `tilemap.h` | S N H | game |
| `TilemapInstance` | component (engine-added) | `tilemap.h` | S N H, custom ops (RLE layers) | `tilemap_sync` (FrameBegin -1000), `set_tile` |
| `GridPosition`, `GridMover` | components | `grid_mover.h` | S N H | Simulate (`try_step`), Animate -1200 |
| `PathRoute` | component | `grid_path.h` | S N H | Simulate -1100 |
| `PathRequest` | component (removed when served) | `path_follow.h` | S N H | game |
| `MapObjectRef` | component (engine-added to spawned objects) | `tilemap.h` | S N H | `tilemap_sync` |
| `TileWorld` | World resource (physics type) | `physics/tile_world.h` | inserted by PhysicsPlugin (or TilemapPlugin if absent) | `tilemap_sync`, `set_tile`, `set_layer`, restore hook (only when a `Tilemap` has `build_collision`) |
| `GridIndex` | World resource | `grid_mover.h` | TilemapPlugin `init_world` | Simulate -1500, `try_step` |
| `PathService` | World resource | `path_follow.h` | TilemapPlugin `init_world` | Simulate -1100 |
| `FovMemory` | World resource (game inserts; `snapshot_resource` it) | `fov.h` | custom ops (RLE grids) | game (Simulate) |
| `FogStyle` | World resource (optional) | `fov.h` | — | game |
| `Tileset`, `TilemapAsset` | assets | `tileset.h`, `tilemap.h` | loaders registered by TilemapPlugin | AssetServer |

The module emits no events. Assets report through `AssetEvent<Tileset>` / `AssetEvent<TilemapAsset>`.

## Systems

| Stage | Scope | Order | System | What it does |
|---|---|---|---|---|
| FrameBegin | PerWorld | -1000 | `tilemap_sync` | Ready/Modified map -> `TilemapInstance`; fills `TileWorld` from the first `Tilemap` with `build_collision` (again when it changes owner, moves, or a tileset reloads); spawns objects as prefabs once |
| TickBegin | PerWorld | -1900 | `tilemap_sync_tick` | the same function at each tick start, so maps added or changed during a tick are in place for the next tick whatever the frame pacing (idempotent) |
| Simulate | PerWorld | -1500 | `grid_index_rebuild` | `GridIndex` from every `GridPosition` |
| Simulate | PerWorld | -1100 | `serve_path_requests` | `PathRequest` -> `PathRoute` within `PathService::searches_per_tick` |
| Animate | PerWorld | -1200 | `grid_movers` | tweens `Transform` between cell centres; `TurnScheduler::busy` while any moves |
| Animate | PerWorld | -1100 | `animated_tiles` | tick clock of animated tiles (presentation) |
| Render | PerWorld | -1000 | `draw_tiles` | visible tiles of visible layers (culled to `CameraView::visible_world`, flips, opacity, parallax, animation) + fog quads into `RenderQueue` |

The world restore hook (undo, loads) re-stamps instances (0 while the map asset is not Ready; the
restored layers are then adopted, not recopied, when it becomes Ready), refills `TileWorld` from the
collision map's instance and rebuilds `GridIndex`.

## Formats

### Tiled `.tmj` / `.tsj`

Orthogonal, finite maps with square tiles. Layer data can be CSV, a JSON array or uncompressed
base64; compressed data gives `Error{Unsupported}`. Group layers are flattened. Supported
properties:

- layer: `collision` (bool, default true), `render_layer` (int)
- tile: `shape`, `hazard`/`ladder`/`water`/`opaque`, `cost` (0 = impassable), `kind` (falls back
  to the tile's class)
- tileset: `atlas` picks the atlas group (`"none"` keeps a standalone texture)

Animations must use consecutive frames. Object layers become `MapObject`s, with tile objects moved
to their top-left corner. Unknown keys are ignored.

### Native `.map.json`

Strict: unknown keys are errors. Keys are `rows` + `legend` (+ `terrains` with props and a visual
`tile`), `autotile` rules (`edge4`, `blob47`, `plain`, variants, blends, details), plain GID
`layers`, `objects`, `properties` and `tilesets`. The header comment in `tilemap.h` has a full
example (a test parses it straight out of the header) and every key with its type and default.

Things that trip people up: legend values are terrain *names*, ASCII art goes in the top-level
`rows` and never in a layer, and objects are `{type, x, y, w, h}` in *cells*. Plain text (`.txt`)
is read as ASCII rows where `#` is a solid opaque wall; it needs no tileset.

### Native `.tileset.json`

Strict. Keys: `image`, `tile_size`, `columns`, `count`, `margin`, `spacing`, `atlas`,
`tiles: {"<id>": {shape, flags, cost, kind, anim}}`.

## Conventions

Grid entities keep `Transform` on the cell centre (`TileWorld::tile_center`) and only move through
`try_step`, `set_grid_position` and `grid_movers`. A path request starts at the `GridPosition`
cell, otherwise at the cell under `Transform.position + PathRequest::start_offset`; pass the same
point to `follow_route`. Feet-anchored bodies pass `Collider::offset` to both, so the body centre
runs through cell centres. Map objects spawn at the map origin + their rect centre (point objects
at the point), or at the bottom centre if they have the property `"anchor": "bottom"`.

Path requests stay pending until they're served at Simulate -1100. If the tick's budget
(`PathService::searches_per_tick`, seeded from `TilemapConfig`) runs out, the request and the old
route are kept for a later tick. Searches that need no A* are free. `computed_tick` is
`World::tick()` at service time, so `route_stale` also notices undo and load.

The first `Tilemap` in view order with `build_collision` owns the world's `TileWorld`. This is
re-evaluated every sync and on restore. If there is none, the module never touches `TileWorld`;
the game fills it and rebuilds it in its own restore hook.

A map that's Ready before a world's first tick is in place on that tick. A `Tilemap` added during
tick N is in place at the start of tick N + 1. Poll `try_get<TilemapInstance>(map)` while it loads.

`try_step` applies immediately (cell, index, tween start). `can_step` is the dry run for undoable
puzzles, and `install_grid_resources` sets up a bare `World` for unit tests.

For generated maps, `AssetServer::add` a `TilemapAsset` built in code (in `Scene::enter`), or carve
a template map with `set_layer`, which refills once per layer instead of once per cell. Custom
object properties reach spawned entities through `MapObjectRef` + `map_object`.

## Notes

- A* (`PathFinder`): integer costs (straight = entered cell's cost, diagonal x1.4, no corner cutting),
  octile/Manhattan heuristic scaled by the cheapest cell, deterministic ties, no allocation after
  `resize`, `Error` if never sized. 64x64: ~0.09 ms typical, ~0.2 ms when the goal is walled
  in (every cell expanded), Release on an M1.
- `DijkstraMap::downhill` follows the cheapest path (parent pointers), so a walk's steps add up to
  `at(p)` exactly; `FlowField` turns it into unit directions.
- FOV is Albert Ford's symmetric shadowcasting with exact integer slopes (if A sees B, B sees A).
- Everything in the tick and frame paths is allocation-free after warm-up (tests assert it with
  `CHECK_NO_ALLOC`); allocations happen at load, on `resize`, and when a map becomes Ready/Modified.
