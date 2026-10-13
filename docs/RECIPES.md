# LambdaEngine2D recipes

A game skeleton, short recipes for common tasks, the usual mistakes, and a map of the example
games. [`GUIDE.md`](GUIDE.md) covers the runtime model behind them (stages, sim/view split, events,
determinism, lifetimes, positions) and [`API_REFERENCE.md`](API_REFERENCE.md) lists every public
declaration. If this file and a header (`src/<module>/include/lam/<module>/*.h`) disagree, the
header wins.

---

## 1. A new game from scratch

The smallest complete game: a platformer hero on a Tiled map with a following camera. The same
code builds as `examples/basics/01_skeleton`, so it keeps compiling as the engine changes.

### 1.1 Files

```
examples/games/hop/               a folder in examples/basics, arcade or games with a CMakeLists.txt
  CMakeLists.txt                  is added by the root CMakeLists (LAM_BUILD_EXAMPLES, ON at top level)
  main.cpp
  assets/                         copied next to the exe; dev builds read the source folder (hot reload)
    prefabs.json                  prefab library; every path inside is relative to assets/
    maps/level1.tmj               Tiled map, uncompressed (CSV or array) layers
    maps/tiles.tsj                Tiled tileset; tile properties give collision shapes and flags
    sprites/tiles.png
    sprites/hero.png              Aseprite export: PNG + JSON (array or hash form); tags = clips
    sprites/hero.json
    sprites/hero.anim.json        animation graph: clips, transitions on params/triggers, frame events
    sfx/game.sounds.json          sound bank: groups of variants (optional)
    fx/spark.particles.json       particle emitter configs (optional; or build them in code)
    data/*.json                   your own tables, read with parse_data + DataReader (optional)
```

### 1.2 `CMakeLists.txt`

```cmake
# One executable linked with lam_engine (the default DEPS). Assets are copied next to it.
lam_add_game(NAME hop SOURCES main.cpp ASSETS assets)
```

`ASSETS` is optional. A game that draws only shapes and the built-in font leaves it out; it then
logs `assets: root ... (fallback)` at startup, which is harmless.

### 1.3 `main.cpp`

```cpp
// Hop: a platformer hero on a Tiled map with a following camera.
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/log.h>
#include <lam/ecs/component.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/physics/motors.h>
#include <lam/render/camera.h>
#include <lam/tilemap/tilemap.h>

struct Player {};                                               // tag component
LAM_REFLECT_TAG(Player)                                         // global scope, right after the type
struct Controls { lam::AxisAction move; lam::ButtonAction jump; };   // App resource: ids resolved once

using namespace lam;
using namespace lam::literals;                                  // "hero"_id

// Startup: runs once on the main world, before frame 1, with the root asset scope current.
static void spawn(Ctx &ctx) {
    World &w = ctx.world();
    Entity map = w.create();
    w.add<Transform>(map);                                      // map origin = (0, 0)
    w.add<Tilemap>(map, Tilemap{.map = ctx.res<AssetServer>().load<TilemapAsset>("maps/level1.tmj")});
    Entity hero = ctx.res<PrefabLibrary>().spawn(w, "hero"_id, {64, 64});
    w.add<Player>(hero);
    Entity cam = w.create();
    w.add<Camera2D>(cam);
    w.add<CameraFollow>(cam, CameraFollow{.target = hero, .half_life = 0.1f, .dead_zone = {-16, -24, 32, 48}});
}

// Stage::Input, every tick: input -> motor intent. Physics moves the body; animation follows.
static void drive(Ctx &ctx) {
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);                    // latched; neutral when unfocused
    for (auto [e, motor] : ctx.world().view<PlatformerMotor>(With<Player>{})) {
        motor.move_x = in.axis2(c.move).x;
        motor.jump_pressed = in.is_pressed(c.jump);
        motor.jump_held = in.is_held(c.jump);
    }
}

int main(int argc, char **argv) {
    App app({.title = "Hop", .virtual_size = {480, 270}}, argc, argv);   // fields in declaration order
    app.add_plugins(DefaultPlugins{});
    app.register_component<Player>();
    ActionMap &map = app.res<ActionMap>();
    app.insert_resource<Controls>(Controls{
        .move = map.axis2("move", {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A),
                                    ActionMap::key(Key::D)}}, {{PadAxis::LeftX, PadAxis::LeftY}}),
        .jump = map.button("jump", {ActionMap::key(Key::Space), ActionMap::pad(PadButton::South)})});
    if (Status s = app.res<PrefabLibrary>().load("prefabs.json", app.serialize_context()); !s) {
        LAM_LOG_ERROR("{}", s.error().message);
        return 1;
    }
    app.add_startup(spawn, "hop.spawn");
    app.add_system(Stage::Input, drive, "hop.drive");
    return app.run();
}
```

The engine supplies the motor (Physics), the animation (`MotorAnimBridge` feeds `speed`, `vx`,
`vy`, `grounded`, `moving`, `climbing` and the facing into the `AnimGraph`), camera follow, tile
drawing and sprite rendering. The main world doesn't tick until the map, sheet and graph are
Ready, so the hero can't fall through an empty `TileWorld`.

`hop --headless --frames 120` should exit 0. With the files of 1.4 and 1.5 (plus a hero sheet with
`idle`, `run` and `jump` tags and a map with a floor), the hero spawned at (64, 64) lands within a
second and the graph goes `jump` then `idle`.

### 1.4 `assets/prefabs.json`

```json
{"prefabs": {"hero": {"components": {
  "Transform": {}, "Velocity": {},
  "Sprite": {"layer": 64},
  "Collider": {"half": [5, 7], "offset": [0, -7]},
  "PlatformerMotor": {},
  "Animator": {"sheet": "sprites/hero.json", "graph": "sprites/hero.anim.json", "clip": "idle"},
  "AnimParams": {}, "MotorAnimBridge": {}}}}}
```

Prefab rules: components are looked up by registered name and must be reflected; missing keys keep
defaults; unknown keys are errors; `"extends": "base"` copies a base prefab (field-level overrides);
`"tags": ["Enemy"]` adds tag components; `"children": [{"prefab": "torch", "offset": [4, -6]}]`
spawns parented children. Vectors and rects are arrays (`[x, y]`, `[x, y, w, h]`), colours are
`"#rrggbb[aa]"` or arrays, enums are their reflected names, asset fields are paths. One bad entry
fails the whole file, and a failed load changes nothing.

### 1.5 The other asset files

`maps/tiles.tsj` gives tiles collision through Tiled custom properties. In the map, a tile layer
contributes to `TileWorld` unless it has the bool property `collision` = false (decoration), and
the int property `render_layer` sets its draw layer (default `kLayerTiles` = 16). Objects spawn the
prefab named by their class (else type), at the point the positions table in
[GUIDE.md](GUIDE.md#3-simulation-and-presentation) gives; objects whose type is not a prefab stay
in `TilemapAsset::objects` for your rules. An example tileset (a 128 x 32 PNG, 8 x 2 tiles of
16 px; the map is a `.tmj` whose tileset `"source"` is `"tiles.tsj"`):

```json
{"type": "tileset", "name": "tiles", "image": "../sprites/tiles.png", "imagewidth": 128, "imageheight": 32,
 "tilewidth": 16, "tileheight": 16, "tilecount": 16, "columns": 8, "margin": 0, "spacing": 0,
 "tiles": [
  {"id": 0, "properties": [{"name": "shape", "type": "string", "value": "solid"}]},
  {"id": 1, "properties": [{"name": "shape", "type": "string", "value": "one_way"}]},
  {"id": 2, "properties": [{"name": "shape", "type": "string", "value": "slope_up_right"}]},
  {"id": 3, "properties": [{"name": "hazard", "type": "bool", "value": true}]},
  {"id": 4, "properties": [{"name": "ladder", "type": "bool", "value": true}]}]}
```

Shapes: `empty`, `solid`, `one_way`, `slope_up_right`, `slope_up_left`, `half_slope_up_right_low`,
`half_slope_up_right_high`, `half_slope_up_left_low`, `half_slope_up_left_high`. Flags (bool):
`hazard`, `ladder`, `water`, `opaque`. Also `cost` (int path cost, 0 = impassable, default 10),
`kind` (a name; else the tile's Tiled type/class) and a Tiled `animation` of consecutive tile ids.
Square tiles only; compressed or infinite maps, `.tsx` tilesets and image collections are refused.
Without Tiled, write a native `.map.json` or a plain-text `.txt` map (recipe 2.8).

`sprites/hero.anim.json` (the `.anim.json` graph; full format in `src/animation/README.md`):

```json
{"sheet": "hero.json", "start": "idle",
 "clips": [{"name": "idle", "tags": ["idle", "idle"]}, {"name": "run", "tags": ["run", "run"]},
           {"name": "jump", "tags": ["jump", "jump"]}],
 "transitions": [{"from": "idle", "to": "run", "param": "moving", "above": 0.5},
                 {"from": "run", "to": "idle", "param": "moving", "below": 0.5},
                 {"from": "*", "to": "jump", "param": "grounded", "below": 0.5},
                 {"from": "jump", "to": "idle", "param": "grounded", "above": 0.5}],
 "events": [{"tag": "run", "frame": 1, "name": "step"}]}
```

Two tags per clip are the right and left facings (the left one is mirrored by default, so naming
the same tag twice is fine). Transitions are tried in file order and the first match wins; a
bare `"param"` means "> 0.5"; `"trigger"` and `"on_finish": true` are the other conditions. The
sheet (`hero.json`, an Aseprite array or hash export with tags) must have every tag the clips
name, and each event frame inside its tag, or the graph fails to load.

`sfx/game.sounds.json` (sound bank; load it with `Audio::add_bank`; full format in
`src/audio/README.md`):

```json
{"bus": "sfx",
 "groups": {"jump": {"file": "jump.wav", "variants": 3, "pitch_jitter": 0.06, "max_voices": 4},
            "coin": {"files": ["coin.wav"], "bus": "ui", "min_interval_ms": 60}}}
```

A group has either `"files"` (a list) or `"file"` plus optional `"variants": n`, which expands to
`jump_1.wav` .. `jump_3.wav` next to the bank. Every file is a dependency: one missing file fails
the whole bank. Other keys: `"volume"` (0..2), `"bus"`, `"pitch_jitter"` (0..0.5),
`"max_voices"`, `"min_interval_ms"`.

`fx/*.particles.json` uses `EmitterConfig` member names as keys (see the particles recipe).

Paths inside an asset file are relative to that file (`.` and `..` allowed): the tileset's
`"image": "../sprites/tiles.png"`, the graph's `"sheet": "hero.json"`, the bank's `"file"`/`"files"`
(next to `game.sounds.json`), a `.tmj` tileset `"source"`, Aseprite `meta.image`. Paths in
`prefabs.json` and in code are relative to `assets/`. Engine formats are strict (Tiled and
Aseprite exports are read leniently): unknown keys are errors, and a failed dependency fails its
parent. When a sprite never animates, check `AssetServer::state(id)` / `error(id)`
([GUIDE.md](GUIDE.md#7-assets)).

### 1.6 Growing it

Once the game has rules worth testing, split it the way the example games do:

```
examples/games/hop/
  CMakeLists.txt
  main.cpp                    presentation: scenes, camera, HUD, fx assets, --bot wiring
  src/hop/components.h        tags, components, resources, events; LAM_REFLECT right after each type
  src/hop/rules.cpp           the rules plugin (below)
  src/hop/bot.cpp             an InputDriver (recipe 2.14)
  tests/CMakeLists.txt
  tests/test_hop.cpp
  assets/...
```

```cmake
lam_add_game(NAME hop
  SOURCES main.cpp
  ASSETS assets
  SIM_LIBRARY hop_sim                                  # include root: ${CMAKE_CURRENT_SOURCE_DIR}/src
  SIM_SOURCES src/hop/rules.cpp src/hop/bot.cpp
  SIM_DEPS lam_app lam_physics lam_tilemap lam_input lam_audio lam_particles   # pure targets only
           lam_render)
if(LAM_BUILD_TESTS AND TARGET lam_test_support)
  add_subdirectory(tests)                              # the root does not add game tests for you
endif()
# tests/CMakeLists.txt:  lam_add_test(MODULE hop SOURCES test_hop.cpp DEPS hop_sim)
#                        -> executable lam_hop_tests, ctest label "hop"
```

Pure targets you may list in `SIM_DEPS`: `lam_math lam_core lam_ecs lam_app lam_assets lam_render
lam_physics lam_input lam_audio lam_scene lam_tilemap lam_animation lam_particles lam_ui`. The
executable links `lam_engine` (all of them plus the raylib backends).

The rules plugin (in `hop_sim`):

```cpp
// src/hop/rules.cpp (in the hop_sim library: pure targets only)
#include <cstdint>
#include <lam/audio/components.h>
#include <lam/core/app.h>
#include <lam/ecs/component.h>
#include <lam/input/input_frame.h>
#include <lam/particles/components.h>
#include <lam/physics/events.h>
#include <lam/physics/motors.h>
#include <lam/render/camera_math.h>                             // ShakeAt

struct Player {};
struct Coin {};
struct Run { std::int32_t score = 0; std::int32_t lives = 3; };   // World resource of the level world
LAM_REFLECT_TAG(Player)
LAM_REFLECT_TAG(Coin)
LAM_REFLECT(Run, score, lives)

namespace hop {
using namespace lam;

void drive(Ctx &ctx);          // Stage::Input        (recipe 2.2)
void collect(Ctx &ctx);        // Stage::PostPhysics  (recipe 2.5)

// Every rule runs only on worlds that hold Run, never on menu or overlay worlds.
static void add_rule(App &app, Stage s, SystemFn fn, std::string_view name, int order) {
    app.add_system({.stage = s, .fn = fn, .name = name, .order = order, .run_if = &has_local<Run>});
}

class RulesPlugin final : public Plugin {
public:
    std::string_view name() const override { return "hop.rules"; }
    void build(App &app) override {
        app.require_plugin("lam.physics", name());
        app.register_component<Player>().register_component<Coin>();
        app.add_event<PlaySound>();                             // idempotent; lets rules run in tests
        app.add_event<BurstAt>();                               // without the audio, particles
        app.add_event<ShakeAt>();                               // and render plugins
        add_rule(app, Stage::Input, drive, "hop.drive", 0);
        add_rule(app, Stage::PostPhysics, collect, "hop.collect", 0);
    }
};

// Called from Scene::enter or a startup system: only the level world holds Run.
void start_level(World &w) {
    w.resources().insert<Run>();
    w.snapshot_resource<Run>();                                 // undo, saves and state_hash cover it
}
}
```

A headless test (links `lam_test_support`, which provides `main`):

```cpp
// tests/test_hop.cpp -- runs without a window (NullPlatform, synchronous assets).
#include <lam/test/support.h>
#include <lam/ecs/world.h>
#include <lam/input/input_plugin.h>
#include <lam/physics/physics_plugin.h>

TEST_CASE("the same seed gives the same state hash") {
    const auto run = [] {
        lam::test::HeadlessApp h({.seed = 7});
        h.add<lam::InputPlugin>();                              // ActionMap: a rules plugin that defines
        h.add<lam::PhysicsPlugin>();                            // actions in build() (2.1) aborts without it
        // h.add<hop::RulesPlugin>(); spawn the level ...
        h.ticks(600);
        return h.app().main_world().state_hash();
    };
    CHECK(run() == run());
}
```

---

## 2. Recipes

Each recipe is a complete snippet and assumes these lines plus the includes it lists:

```cpp
#include <lam/core/app.h>
#include <lam/ecs/commands.h>                                   // ctx.commands() needs the full type
#include <lam/ecs/component.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
struct Player {};
LAM_REFLECT_TAG(Player)
struct Controls { lam::AxisAction move; lam::ButtonAction jump, fire, pause, undo; };   // App resource
using namespace lam;
using namespace lam::literals;                                  // "name"_id
```

Register every system with the stage named in its comment, for example
`app.add_system({.stage = Stage::PostPhysics, .fn = collect_coins, .name = "game.coins", .run_if = &has_local<Run>});`.

### 2.1 Input actions and rebinding

```cpp
#include <lam/input/actions.h>
#include <lam/input/keys.h>

// After app.add_plugins(DefaultPlugins{}). Names resolve to ids once; keep the ids in a resource.
void define_controls(App &app) {
    ActionMap &map = app.res<ActionMap>();
    const auto k = [](Key key) { return ActionMap::key(key); };
    Controls c;
    c.move = map.axis2("move",
                       {{k(Key::W), k(Key::S), k(Key::A), k(Key::D)},           // up, down, left, right
                        {k(Key::Up), k(Key::Down), k(Key::Left), k(Key::Right)}},
                       {{PadAxis::LeftX, PadAxis::LeftY}});
    c.jump = map.button("jump", {k(Key::Space), ActionMap::pad(PadButton::South)});
    c.fire = map.button("fire", {ActionMap::mouse(MouseButton::Left), ActionMap::pad(PadButton::RB)});
    c.pause = map.button("pause", {k(Key::Escape), ActionMap::pad(PadButton::Start)});
    c.undo = map.button("undo", {k(Key::Z), k(Key::Backspace)});
    map.set_repeat(c.undo, 18, 4);                              // is_repeated: press, then every 4 ticks after 18
    app.insert_resource<Controls>(c);
}

// Tick system (Stage::Input or Simulate): the latched InputFrame. Each press reaches one tick.
void read_controls(Ctx &ctx) {
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);                    // neutral if this world is not focused
    const Vec2 move = in.axis2(c.move);                         // |move| <= 1, +y is down
    const bool jump = in.is_pressed(c.jump);                    // also is_held / is_released / is_repeated
    const Vec2 aim = in.pointer_world;                          // mouse in world pixels (pointer_valid)
    (void)move; (void)jump; (void)aim;
}

// Frame system (Update), e.g. a "press a key" button on your own options screen.
void start_rebind_jump(Ctx &ctx) {
    ctx.res<ActionMap>().begin_capture(ctx.res<Controls>().jump, /*player*/ 0, /*slot*/ 1);
}
```

`InputPlugin` saves bindings to the settings file as `input.p<player>.<action>.<slot>` and loads
them at startup. The engine settings scene (`make_settings_scene()`) already offers rebinding.
Menus and UI read `frame_input(ctx)` (unlatched, frame time) instead of `input_frame`.

### 2.2 Player movement: top-down and platformer motors

```cpp
#include <lam/physics/components.h>
#include <lam/physics/motors.h>

// A motor body needs Transform + Velocity + a Box Collider + the motor (adding a motor or a
// RigidBody also adds Velocity). Usually from a prefab; in code it looks like this (outside any
// view, or through ctx.commands()):
Entity spawn_walker(World &w, Vec2 at) {
    const Entity e = w.create();
    w.add<Transform>(e, Transform{.position = at});
    w.add<Velocity>(e);
    w.add<Collider>(e, Collider{.offset = {0, -4}, .half = {5, 4}, .layer = 2});   // feet box
    w.add<TopDownMotor>(e, TopDownMotor{.max_speed = 90});
    w.add<Player>(e);
    return e;
}

// Top-down (Stage::Input): write intent only. The motors system (Physics -1500) moves the body
// against tiles and solid colliders, with corner assist.
void top_down_input(Ctx &ctx) {
    const Vec2 move = input_frame(ctx).axis2(ctx.res<Controls>().move);
    for (auto [e, motor] : ctx.world().view<TopDownMotor>(With<Player>{}))
        motor.move = clamp_length(move, 1.0f);
}

// Platformer (Stage::Input): run, jump (buffer + coyote time are built in), drop, climb.
void platformer_input(Ctx &ctx) {
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    const Vec2 axis = in.axis2(c.move);
    for (auto [e, motor] : ctx.world().view<PlatformerMotor>(With<Player>{})) {
        motor.move_x = axis.x;
        motor.climb_y = axis.y;                                 // ladders; +y is down
        motor.drop = axis.y > 0.5f;                             // through one-way platforms
        motor.jump_pressed = in.is_pressed(c.jump);
        motor.jump_held = in.is_held(c.jump);                   // release early: jump_cut
    }
}

// Reading motor state (written by physics) is fine anywhere after Physics, e.g. PostPhysics.
bool hero_grounded(World &w) {
    for (auto [e, motor] : w.view<PlatformerMotor>(With<Player>{}))
        if (motor.on_ground) return true;
    return false;
}
```

Tune feel with the motor fields (`run_speed`, `accel`, `jump_speed`, `gravity`, `coyote_ticks`,
`buffer_ticks`, `corner_assist`, ...), usually in the prefab. `solid_mask` picks the collider
layers that block the body (non-trigger Box colliders); tiles always block, and `solid_mask = 0`
means tiles only. Enemies can reuse motors: an AI rule writes the same intent fields.

Spawn motor bodies clear of solid tiles (`nearest_free_spot` finds a place). A top-down body that
starts inside a wall can only move out of it, and a platformer body there doesn't move at all. A
world with no map (empty `TileWorld`, 0 x 0 cells) has no tile collision whatever `outside_solid`
says, so a code-only game's bodies are blocked only by colliders. Top-down `corner_assist` slides
round a corner only while the input across the motion is exactly 0 (4-way digital input); AI and
crowds should steer from the body centre (`position + Collider::offset`) toward cell centres.

Intent fields are level-triggered: the motors read them every tick and never clear them, so a body
keeps walking on the last `move` until your code writes a new one. Write them every tick. To stop a
body (game over, cutscene), zero its intent fields and `Velocity`; a `run_if` that skips your input
system leaves the motor running on the last intent. `drop` means "down held" (falls through one-way
platforms while true); `climb_y` -1 is up, and only a non-zero `climb_y` grabs a ladder. For a
fixed-speed shove (dash, knockback) use `move_body` from `physics/queries.h`. To move an entity
without a visible glide, use `World::teleport` or `Commands::teleport`.

### 2.3 Camera follow and shake

```cpp
#include <lam/render/camera.h>
#include <lam/render/camera_math.h>                             // ShakeAt

// Camera components are presentation (frame time, never hashed). Put them on their own entity.
Entity add_camera(World &w, Entity target, Rect level_bounds) {
    const Entity cam = w.create();
    w.add<Camera2D>(cam);
    w.add<CameraFollow>(cam, CameraFollow{.target = target, .half_life = 0.1f, .lookahead_seconds = 0.15f,
                                          .lookahead_max = 24, .dead_zone = {-16, -24, 32, 48}});
    w.add<CameraBounds>(cam, CameraBounds{.world = level_bounds, .enabled = true});
    w.add<CameraShake>(cam, CameraShake{.max_offset = 4});
    return cam;
}

// Any tick stage: rules shake the camera through the engine event ShakeAt, never by writing
// CameraShake. camera_shake (Update -1200) adds the trauma to every CameraShake of the emitting
// world before it decays, so the frame that draws this tick already shakes. RenderPlugin
// registers ShakeAt; a rules plugin that must also run without it calls app.add_event<ShakeAt>().
void on_explosion(const Ctx &ctx) { ctx.emit(ShakeAt{.trauma = 0.4f}); }

// Aim 8 px above the hero's feet with an invisible child anchor (Parent::local is the offset);
// follow the anchor instead of the hero. CameraFollow::offset = {0, -8} does the same.
Entity add_aim_anchor(World &w, Entity hero) {
    const Entity anchor = w.create();
    w.add<Transform>(anchor);
    set_parent(w, anchor, hero, Transform{.position = {0, -8}});
    return anchor;
}

// Retarget after a respawn or teleport without a long glide.
void retarget(World &w, Entity cam, Entity hero) {
    CameraFollow &f = w.get<CameraFollow>(cam);
    f.target = hero;
    f.snap_next = true;
}
```

The highest `Camera2D::priority` in a world wins. A world without a `Camera2D` (a screen-space
overlay) gets the default view, where world pixels are virtual pixels. A hero spawned by a Tiled
object doesn't exist yet during `Scene::enter` (objects spawn at the world's first `tilemap_sync`),
so attach `CameraFollow::target` from an Update system at order -1350 once it does, as
`examples/games/06_platformer` does. A target without a `Transform` leaves the camera where it is,
and the first frame with one snaps. `CameraFollow` aims at the target's interpolated feet plus
`CameraFollow::offset` (world px). `CameraBounds` keeps the view inside `world` and centres an axis
where the level is smaller than the view. Trauma is clamped to 1; `ShakeAt` is presentation only, so
replays and state hashes never see it.

### 2.4 Enemy AI: state machine, path following, steering

```cpp
#include <cstdint>
#include <lam/audio/components.h>
#include <lam/core/fsm.h>
#include <lam/physics/components.h>
#include <lam/physics/motors.h>
#include <lam/physics/steering.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/path_follow.h>

enum class Mood : std::uint8_t { Idle, Chase, Bite };
struct Wolf {                                                   // component (prefab: "Wolf": {})
    Fsm<Mood> fsm;
    float sight = 140;                                          // px
    std::uint64_t route_tick = 0;                               // world tick of the last PathRequest
};
// Enum names first, then the Fsm instantiation, then the owner. Global scope, after the types.
LAM_REFLECT_ENUM(Mood, "idle", "chase", "bite")
LAM_REFLECT_STRUCT(lam::Fsm<Mood>, state, previous, ticks_in_state, entered)
LAM_REFLECT(Wolf, fsm, sight, route_tick)

// Stage::Simulate. The wolf prefab has Transform, Velocity, Collider (feet-anchored: offset
// {0, -h/2}), TopDownMotor, PathRoute and Wolf.
void wolf_brains(Ctx &ctx) {
    World &w = ctx.world();
    const TileWorld &tiles = ctx.local<TileWorld>();
    Vec2 target, target_eye;
    bool found = false;
    for (auto [e, tf, col] : w.view<Transform, Collider>(With<Player>{})) {
        target = tf.position;
        target_eye = sight_point(collider_box(col, tf.position));   // absolute box -> eye
        found = true;
    }
    if (!found) return;
    const std::uint64_t now = w.tick();
    for (auto [e, wolf, tf, col, motor, route] : w.view<Wolf, Transform, Collider, TopDownMotor, PathRoute>()) {
        const bool sees = can_see(tiles, sight_point(collider_box(col, tf.position)), target_eye, wolf.sight);
        const Vec2 centre = tf.position + col.offset;           // the point routes are walked with
        Vec2 dir;                                               // unit intent for the motor
        switch (wolf.fsm.state) {
        case Mood::Idle:
            if (sees) wolf.fsm.set(Mood::Chase);
            break;
        case Mood::Chase:
            if (distance(tf.position, target) < 10) {
                wolf.fsm.set(Mood::Bite);                       // act on the transition itself
                ctx.emit(PlaySound{.group = "bite"_id, .position = tf.position, .spatial = true});
            } else if (sees) {
                dir = seek(tf.position, target, 1.0f);          // straight at it (speed 1 = unit)
            } else {                                            // round walls: A* served in Simulate -1100
                const IVec2 goal = tiles.tile_at(target);
                if (!w.has<PathRequest>(e) && route_stale(route, goal, now, wolf.route_tick, 30)) {
                    ctx.commands().add(e, PathRequest{.goal = goal, .requested_tick = now,
                                                      .start_offset = col.offset});
                    wolf.route_tick = now;
                }
                dir = follow_route(route, tiles, centre);       // unit direction or zero
            }
            if (!sees && wolf.fsm.ticks_in_state > 600) wolf.fsm.set(Mood::Idle);
            break;
        case Mood::Bite:                                        // ticks_in_state is 1 on its first run
            if (wolf.fsm.ticks_in_state >= 30) wolf.fsm.set(Mood::Chase);
            break;
        }
        motor.move = dir;                                       // speed comes from TopDownMotor::max_speed
        wolf.fsm.tick();                                        // once per tick, after the state logic
    }
}
```

- A `PathRequest` added through commands is served by `serve_path_requests` (Simulate -1100) on
  the next tick, or later when that tick's budget (`PathService::searches_per_tick`, default 8)
  is spent; meanwhile the old `PathRoute` stays, so the walker keeps walking. The result is
  written into the entity's `PathRoute` (added if missing, `computed_tick` = `World::tick()`) and
  the request is removed. The search starts from the `GridPosition` cell, else the cell under
  `Transform.position + start_offset`. Check `has<PathRequest>` before asking again;
  `route_stale` also catches routes "from the future" after an undo or load.
- `start_offset = col.offset` and `follow_route(..., position + col.offset)` make the body
  centre run through cell centres. With zero offsets the feet do, and a body taller than half
  a tile scrapes the wall above in one-tile corridors.
- Other steering helpers (`physics/steering.h`): `flee`, `hold_ring` (circle at a distance),
  `pick_wander_goal`, `box_path_clear`. Their `box` is the body's box relative to the position
  arguments (`collider_box(col, {0, 0})`), and `pick_wander_goal` writes an anchor (the new
  `Transform.position`). `can_see` and `tile_ray_clear` are pure `TileWorld` tests, valid in any
  stage. Crowds: one `DijkstraMap` + `FlowField` (`tilemap/grid_maps.h`) for all enemies, plus
  `separation` (`physics/queries.h`; collider centres, optional layer mask).
- Hitboxes from animation: `AnimationEvent` (emitted at Animate -1400) is read in Simulate on the
  next tick; `current_slice(animator, sheet, "hit"_id, flip_x)` gives the Aseprite slice rect.

### 2.5 Collision response

```cpp
#include <cstdint>
#include <lam/audio/components.h>
#include <lam/physics/components.h>
#include <lam/physics/events.h>

struct Coin {};                                                 // tag on a trigger collider
struct Enemy {};
struct Health { std::int32_t hp = 3; };
struct Score { std::int32_t value = 0; };                       // World resource (snapshot_resource it)
LAM_REFLECT_TAG(Coin)
LAM_REFLECT_TAG(Enemy)
LAM_REFLECT(Health, hp)
LAM_REFLECT(Score, value)

// All three run in Stage::PostPhysics: contact events are emitted at Physics -1000 (ProjectileHit
// at -1300) and PostPhysics reads them in the same tick. Structural changes go through commands.
void collect_coins(Ctx &ctx) {
    World &w = ctx.world();
    for (const TriggerEntered &t : ctx.read<TriggerEntered>()) {
        if (!w.has<Coin>(t.trigger) || !w.has<Player>(t.other)) continue;
        ctx.local<Score>().value += 10;
        ctx.emit(PlaySound{.group = "coin"_id, .position = w.get<Transform>(t.trigger).position});
        ctx.commands().destroy(t.trigger);
    }
}

// CollisionStarted orders the pair by entity index (a.index < b.index), normal from a to b.
// other(c, self) and normal_from(c, self) read it from your entity's side; never swap by hand.
void stomp_enemies(Ctx &ctx) {
    World &w = ctx.world();
    for (const CollisionStarted &c : ctx.read<CollisionStarted>()) {
        const Entity hero = w.has<Player>(c.a) ? c.a : c.b;
        if (!w.has<Player>(hero)) continue;
        const Entity enemy = other(c, hero);
        if (!w.has<Enemy>(enemy)) continue;
        if (normal_from(c, hero).y > 0.5f) ctx.commands().destroy(enemy);   // hero landed on top
    }
}

void bullet_hits(Ctx &ctx) {
    World &w = ctx.world();
    for (const ProjectileHit &hit : ctx.read<ProjectileHit>()) {
        ctx.commands().destroy(hit.projectile);                 // the game decides the bullet's fate
        if (hit.tile || !w.alive(hit.target)) continue;
        Health *h = w.try_get<Health>(hit.target);
        if (h != nullptr && h->hp > 0 && --h->hp == 0) ctx.commands().destroy(hit.target);
    }
}
```

- A trigger is a `Collider` with `trigger = true`: it reports overlaps and never blocks.
- Body type: an entity with `RigidBody` uses its `type`; a `Collider` without `RigidBody` is
  Kinematic if it has `Velocity` or a mover component (a motor, `MovingPlatform`, `PathFollower`,
  `Projectile`), otherwise Static. A Kinematic body with only `Velocity` moves by it every tick
  (no gravity, no tile or collider blocking, no push-out) and still reports contacts and
  triggers: the basis of any scripted mover. Give every mover a `Velocity` anyway: a `MovingPlatform`
  without a `PathFollower` moves by it, `PathFollower` writes it, and dynamic bodies read it as
  the mover's speed on contact.
- `layer`/`mask` filter pairs both ways: colliders a and b interact only when
  `(a.layer & b.mask) != 0 && (b.layer & a.mask) != 0`. Reporting is separate from blocking: an
  interacting pair reports contacts whatever the body types (Static-Static excepted), so two motor
  bodies that do not block each other still get `CollisionStarted` (stomps work). A motor's
  `solid_mask` decides only what blocks it.
- Static bodies (a `Collider` alone) are never moved by physics, so they never settle: place a
  static crate exactly on the floor.
- `TileHazardTouched{entity, tile}` reports hazard tiles once per body per tick, every tick while
  it touches (no started/ended pair): debounce in your rules with invulnerability ticks.
- `ProjectileHit` (Physics -1300): a projectile hits non-trigger colliders whose layer is in its
  own `mask`, never its `owner`. Each hit spends one `pierce`; the hit after the last one stops it
  at the contact point with zero `Velocity`, as does the first blocking tile when `stop_at_tiles`
  (`hit.tile` true, null target). A spent projectile rests there and reports nothing more;
  physics never destroys it: your rule does (as above), or a `Lifetime` bounds its range. A shot
  spawned overlapping a target hits it on its first tick (point-blank shots work).
- Shapes within 1/32 px count as touching, so a crate resting on another reports one
  `CollisionStarted` and doesn't flicker. Undo and load recompute the contact set, so nothing
  fires that didn't change.
- Spatial queries (`overlap_box`, `overlap_circle`, `raycast`, `sweep_circle`, `query_neighbors`,
  `separation`) are correct in any tick stage and after undo or load: the first query of a tick
  (or any spawn, destroy or body component change) rebuilds their grids. A body you teleport by
  writing `Transform` after that first query can be missed later in the tick; query first, or call
  `refresh_queries(w)` after the move. `RayQuery` turns tiles off or triggers on.

A bouncing ball can be left to physics: a Dynamic body with
`RigidBody{.restitution = 1, .friction = 0, .gravity_scale = 0}` bounces off tiles and colliders
on its own (plain mirror angle). For control over the angle, as Breakout does off the paddle, use
a Kinematic ball (`Collider` + `Velocity`, no `RigidBody`) and a rule that reflects it:

```cpp
#include <lam/physics/components.h>
#include <lam/physics/events.h>

struct Ball {};
LAM_REFLECT_TAG(Ball)

// Stage::PostPhysics. One tick can bring several contacts (a wall, then the paddle's edge), and a
// continuing touch never reports again. Reflecting per event lets the second undo the first, so
// sum the tick's normals and reflect once, only when moving into them.
void bounce_balls(Ctx &ctx) {
    World &w = ctx.world();
    for (auto [ball, v] : w.view<Velocity>(With<Ball>{})) {
        Vec2 into;                                              // toward what the ball touched
        for (const CollisionStarted &c : ctx.read<CollisionStarted>())
            into += normal_from(c, ball);                       // zero for pairs without this ball
        const Vec2 n = normalize_or(into, {});
        const float speed_in = dot(v.linear, n);
        if (speed_in > 0.0f) v.linear -= n * (2.0f * speed_in);
    }
}
```

### 2.6 Spawning: prefabs, `spawn_prefab`, `Lifetime`

```json
{"prefabs": {
  "bullet": {"tags": ["Bullet"], "components": {
    "Transform": {}, "Velocity": {}, "Projectile": {"radius": 2, "mask": 4}, "Lifetime": {"ticks": 90},
    "Sprite": {}, "Animator": {"sheet": "sprites/bullet.json", "clip": "fly"}}},
  "grunt": {"tags": ["Enemy"], "components": {
    "Transform": {}, "Velocity": {}, "Collider": {"half": [6, 6], "layer": 4},
    "TopDownMotor": {"max_speed": 40}, "Health": {"hp": 3},
    "Sprite": {}, "Animator": {"sheet": "sprites/grunt.json", "clip": "walk"}}},
  "brute": {"extends": "grunt", "components": {"TopDownMotor": {"max_speed": 25}, "Health": {"hp": 10}}}
}}
```

```cpp
#include <lam/ecs/lifetime.h>
#include <lam/ecs/prefab.h>
#include <lam/math/vec2.h>
#include <lam/physics/components.h>

// Stage::Simulate: fire toward the mouse. spawn_prefab records the prefab's components; records
// added after it for the same entity win (assigned at apply, in record order).
void fire(Ctx &ctx) {
    const InputFrame &in = input_frame(ctx);
    if (!in.is_pressed(ctx.res<Controls>().fire) || !in.pointer_valid) return;
    Commands &cmd = ctx.commands();
    for (auto [e, tf] : ctx.world().view<Transform>(With<Player>{})) {
        const Vec2 dir = normalize_or(in.pointer_world - tf.position, {1, 0});
        const Entity b = cmd.spawn_prefab("bullet"_id, tf.position);
        cmd.add(b, Velocity{dir * 240.0f});
        cmd.add(b, Projectile{.radius = 2, .mask = 4, .owner = e});
        cmd.add(b, Lifetime{.ticks = 90});                      // destroyed at TickEnd -900 after 1.5 s
    }
}

// Immediate spawn (startup, Scene::enter, or a system outside any view): returns the live root.
Entity spawn_boss(Ctx &ctx, Vec2 at) {
    return ctx.res<PrefabLibrary>().spawn(ctx.world(), "brute"_id, at);
}
```

The `Bullet` and `Enemy` tags must be registered game components (`LAM_REFLECT_TAG` +
`register_component`). Spawned entities appear in place, without an interpolation glide.
`Lifetime` counts ticks and destroys the entity's children too. Physics stops a `Projectile` at its
last hit (zero `Velocity`) but never destroys it; do that on `ProjectileHit` as in recipe 2.5.

### 2.7 Particles via `BurstAt` and sound via `PlaySound`

```cpp
#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/audio/components.h>
#include <lam/math/color.h>
#include <lam/particles/components.h>
#include <lam/particles/particles.h>

struct Fx { AssetId<EmitterConfig> spark, dust; };              // App resource, filled by presentation

// main(), after add_plugins: a runtime config, a file config, and a sound bank.
void load_fx(App &app) {
    AssetServer &assets = app.res<AssetServer>();
    EmitterConfig c;
    c.capacity = 128;
    c.speed_min = 40;
    c.speed_max = 110;
    c.life_min = 0.2f;
    c.life_max = 0.45f;
    c.color_start = Color::hex(0xFFD54AFF);
    c.color_end = Color::hex(0xFF600000);
    c.gravity = {0, 200};
    app.insert_resource<Fx>(Fx{.spark = assets.add<EmitterConfig>(sanitize(c), "fx/spark"),
                               .dust = assets.load<EmitterConfig>("fx/dust.particles.json")});
    if (Audio *audio = app.try_res<Audio>()) audio->add_bank(assets.load<SoundBank>("sfx/game.sounds.json"));
}

// Any tick stage: describe what happened. particles_update (Update -1000) and audio_world
// (Update -900) turn the events into particles and voices for this world.
void on_hit(const Ctx &ctx, Vec2 at) {
    if (const Fx *fx = ctx.try_res<Fx>()) ctx.emit(BurstAt{.config = fx->spark, .position = at, .count = 12});
    ctx.emit(PlaySound{.group = "hit"_id, .position = at, .spatial = true, .volume = 0.8f});
}

// Spatial sounds are mixed relative to this world's AudioListener (put it on the player).
void add_listener(World &w, Entity player) { w.add<AudioListener>(player, AudioListener{.range = 320}); }

// Music is presentation: call Audio from a frame stage (or main), never from a tick system.
void start_music(Ctx &ctx) {                                    // e.g. Update, once
    if (Audio *audio = ctx.try_res<Audio>()) audio->play_music(ctx.res<AssetServer>().load<MusicTrack>("music/theme.ogg"));
}
```

- Continuous effects: a `ParticleEmitter{.config = id}` component on an entity with a `Transform`.
  Tick systems may set its `active` or `pending_burst`; `burst(emitter, n)` does the latter.
  Weather: `assets.add(rain_preset(0.7f))` on an emitter entity (screen space, no Transform).
- `.particles.json` keys are `EmitterConfig` member names, for example
  `{"capacity": 256, "rate": 70, "speed_min": 12, "speed_max": 34, "life_min": 0.8, "life_max": 1.6,
  "color_start": "#ffd080ff", "color_end": "#ff300000", "gravity": [0, -20]}`. Also `"sprite"`
  (`circle`, `square`, `falloff`) or `"image"` (a PNG next to the JSON file) with `"source"` and
  `"atlas"`, `"spawn_shape"` (`rect`, `ellipse`, `ring`) and `"layer"` as a number or a name.
  Unknown keys are load errors (`src/particles/README.md` has the full example).
- `BurstAt::count` 0 means the config's `burst`. A burst whose config is not Ready is dropped.
- Looping positional sounds: an `AudioEmitter{.group = ..., .loop = true}` component.
- `AudioListener` needs a `Transform`. Each world's first such entity is where that world's
  spatial sounds are heard from; a world without one plays them unpanned at full volume.
- Sim code that must also run in tests without these plugins: register `PlaySound`/`BurstAt`
  (and `ShakeAt`, recipe 2.3) in your plugin (`add_event` is idempotent; otherwise `emit` aborts)
  and keep effect ids in an optional App resource like `Fx`, filled by presentation and checked
  before emitting, as above. Events are never hashed, so asset ids inside them are safe.

### 2.8 Tilemaps: Tiled and native maps

```cpp
#include <string_view>
#include <lam/assets/asset_server.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/tilemap.h>

// Startup or Scene::enter. tilemap_sync (FrameBegin -1000, and TickBegin -1900 for maps added or
// changed during a tick) copies the layers into a per-world TilemapInstance, fills this world's
// TileWorld from the collision layers and spawns each object as the prefab named by its type,
// once. Ticks wait until startup and scene assets are Ready (wait_for_startup_assets, wait_for_assets).
// path: "maps/level1.tmj" (Tiled), "maps/cave.map.json" (native) or "maps/test.txt" (ASCII rows).
Entity add_map(Ctx &ctx, std::string_view path) {
    World &w = ctx.world();
    const Entity map = w.create();
    w.add<Transform>(map);                                      // position = map origin (top-left of cell 0, 0)
    w.add<Tilemap>(map, Tilemap{.map = ctx.res<AssetServer>().load<TilemapAsset>(path)});
    return map;
}

// Queries: TileWorld is a World resource (physics inserts it; tilemap fills it).
bool solid_at(const Ctx &ctx, Vec2 p) {
    const TileWorld &tw = ctx.local<TileWorld>();
    return tw.at(tw.tile_at(p)) == TileShape::Solid;
}
bool hazard_at(const Ctx &ctx, Vec2 p) {
    const TileWorld &tw = ctx.local<TileWorld>();
    return (tw.flags_at(tw.tile_at(p)) & kTileHazard) != 0;
}

// Custom properties of the map object that spawned an entity (a Tiled door with "locked": true).
bool door_locked(const Ctx &ctx, Entity door) {
    const MapObjectRef *ref = ctx.world().try_get<MapObjectRef>(door);
    const MapObject *obj = ref ? map_object(ctx.world(), ctx.res<AssetServer>(), *ref) : nullptr;
    return obj != nullptr && prop_bool(obj->props, "locked"_id);
}

// Edits (tick stage): change this world's instance and TileWorld, never the shared asset.
void break_block(World &w, Entity map, IVec2 cell) { set_tile(w, map, "ground"_id, cell, 0); }
```

A native map (`maps/cave.map.json`, strict: unknown keys are errors) needs no Tiled. Every
character of `rows` is a legend key; terrains give collision, path cost and the visual tile:

```json
{"tile_size": 16,
 "tilesets": ["tiles.tsj"],
 "terrains": {"wall": {"shape": "solid", "flags": ["opaque"], "cost": 0, "tile": 1},
              "floor": {"tile": 9},
              "water": {"cost": 30, "flags": ["water"], "tile": 10}},
 "legend": {"#": "wall", ".": "floor", "~": "water",
            "@": {"terrain": "floor", "object": "hero"}},
 "rows": ["##########",
          "#@...~~..#",
          "#....~~..#",
          "##########"],
 "properties": {"music": "cave"}}
```

- The parser makes a hidden collision layer `"terrain"` from the rows and a visible `"ground"`
  layer from the `"tile"` GIDs (GIDs of the listed tilesets, paths relative to the map file), so
  collision, costs and `set_tile` work as for Tiled maps. A terrain named only in the legend is
  Empty with cost 10. `"@": {"terrain": ..., "object": "hero"}` adds a 1 x 1-cell object (spawned
  at the cell centre). Other keys (`autotile`, `layers`, `objects`, `width`/`height`) are listed
  with their defaults in `tilemap/tilemap.h`.
- A plain `.txt` (or `.map.txt`) file of ASCII rows also loads as a `TilemapAsset`: every
  character but space is a terrain named by itself, and `#` is a solid, opaque, impassable wall.
  It needs no tileset and still fills `TileWorld` (nothing is drawn).
- Objects spawn the prefab named by their class (else type): a rectangle or 1 x 1 native object
  at its centre, a point object at the point, an object with the custom property
  `"anchor": "bottom"` at the bottom centre (feet-anchored prefabs). Each spawned entity gets
  `Name` (the object's name) and `MapObjectRef`; a prefab with `GridPosition` gets the cell under
  the object's centre. Objects whose type is not a prefab stay in `TilemapAsset::objects`.
- Timing: a map that is Ready before a world's first tick has its `TilemapInstance`, `TileWorld`
  and spawned objects on that tick; a `Tilemap` added during tick N has them at the start of tick
  N + 1. While its asset loads it has none of them: poll `try_get<TilemapInstance>(map)` before
  relying on `TileWorld`.
- `TileWorld::outside_solid` is true by default: cells outside a non-empty map are solid, so a
  pit needs its own "fell out" line in your rules, or `open_sides = kSideBottom` (cells below the
  map are then empty). It only applies around a map: a `TileWorld` with no cells blocks nothing.
- `Tilemap{.map = ..., .build_collision = false}` draws a map but leaves `TileWorld` to your rules
  (hand-built levels). Fill it with `TileWorld::resize` and `set`; `set` leaves `cost` at its
  value (10 after `resize`), so write `cost(x, y) = 0` for walls too. Rebuild it in your own
  `World::add_restore_hook` (once per world, from `Plugin::init_world`), because undo and load
  only restore what is snapshotted. With `build_collision`, the first `Tilemap` of the world (view
  order) owns `TileWorld` and the tilemap module refills it after undo and load.
- Generated levels: build a `TilemapAsset` in code and `AssetServer::add` it in `Scene::enter`,
  or carve a template map with `set_layer` (one refill instead of one per `set_tile`). A
  hot-reloaded or `mark_modified` map recopies the instance and discards `set_tile` edits.
- Other tilemap tools: `autotile`, `update_fov` + `FovMemory` + `FogStyle` (fog of war;
  `compute_fov` is the raw callback form), `bsp_rooms` / `cellular_caves` / `connect_regions`
  (procgen), `PathFinder`, `DijkstraMap`, `FlowField`.

### 2.9 Scene switch and an overlay pause menu

```cpp
#include <cstdint>
#include <memory>
#include <lam/ecs/prefab.h>
#include <lam/scene/scene.h>
#include <lam/ui/menu_scenes.h>

struct Run { std::int32_t score = 0; std::int32_t level = 1; };   // World resource of the play world
LAM_REFLECT(Run, score, level)

class PlayScene final : public Scene {
public:
    explicit PlayScene(std::int32_t level = 1) : level_(level) {}
    std::string_view name() const override { return "play"; }
    // A fresh World; the scene's asset scope is current, so loads here unload with the scene.
    void enter(App &app, World &world) override {
        world.resources().insert<Run>(Run{.level = level_});
        world.snapshot_resource<Run>();                         // undo, saves and state_hash cover it
        app.res<PrefabLibrary>().spawn(world, "hero"_id, {64, 64});
    }
    // The overlay above was popped; msg is the menu's result (valid during the call only).
    void resumed(App &app, const SceneMessage *msg) override {
        if (msg == nullptr) return;
        if (msg->kind == "quit"_id) app.quit();
        if (msg->kind == "restart"_id)
            app.res<SceneStack>().replace(std::make_unique<PlayScene>(level_),
                                          Transition{.kind = TransitionKind::Fade});
    }
private:
    std::int32_t level_;
};

// Startup (main world): open the first scene. From then on the world stack holds scene worlds
// only: the main world stops ticking and rendering until the scene stack is empty again.
void start(Ctx &ctx) { ctx.res<SceneStack>().push(std::make_unique<PlayScene>()); }

// Stage::Input, gated with run_if = has_local<Run> so it never runs in the menu's own world.
// Pushed from a tick stage, so it applies at the next tick boundary and replays.
void pause_on_key(Ctx &ctx) {
    if (!input_frame(ctx).is_pressed(ctx.res<Controls>().pause)) return;
    MenuSpec spec;
    spec.title = "PAUSED";
    spec.items.push_back(MenuItem{.id = "resume"_id, .label = "Resume"});
    spec.items.push_back(MenuItem{.id = "restart"_id, .label = "Restart"});
    spec.items.push_back(MenuItem{.id = "quit"_id, .label = "Quit"});
    ctx.res<SceneStack>().push(std::make_unique<MenuScene>(std::move(spec)));   // overlay, blocks ticks
}

void register_flow(App &app) {
    app.add_startup(start, "game.start");
    app.add_system({.stage = Stage::Input, .fn = pause_on_key, .name = "game.pause", .run_if = &has_local<Run>});
}
```

- `push` (on top), `pop(SceneMessage)` (return a result to `resumed`), `replace` (swap the top,
  e.g. next level), `reset` (clear the stack, e.g. back to title). Transitions are measured in ticks;
  a second transition waits for the first.
- `overlay()` keeps lower scenes drawn; `blocks_ticks()` (default true) freezes them;
  `blocks_input()` (default true) takes focus. Subclass `MenuScene` to override `blocks_ticks()`
  if the world below should keep playing (a title-screen demo).
- A selected menu item pops the menu with `SceneMessage{kind = item id, value = item value}`; the
  back action pops with `kind = "back"`. Selections travel as intents, so menus replay.
- `resumed()` runs inside the scene apply that performs the pop (FrameBegin -2000 or TickBegin
  -2000, whichever comes first). Stack operations issued there (`replace` above) are queued and run
  in the same apply, after the current one. Stick to stack operations and intents there rather
  than changing worlds directly.
- Gameplay in the main world cannot sit under a menu: the first `push` takes the main world out of
  the stack. Put gameplay in a `Scene` from the start, as `start` does.
- To restart, `SceneStack::reset`/`replace` with a new scene rather than resetting a world by
  hand; each scene gets a fresh World and asset scope.
- Simple pause without scenes: `App::set_paused(true)` holds ticks (frame stages still run).

### 2.10 UI: HUD, buttons and intents

```cpp
#include <cstdint>
#include <lam/core/fixed_string.h>
#include <lam/input/driver.h>
#include <lam/ui/ui.h>

struct Run { std::int32_t score = 0; std::int32_t potions = 0; };   // sim state (snapshot_resource)
LAM_REFLECT(Run, score, potions)
struct Hud { UiId score = 0; std::int32_t shown = -1; };        // presentation; store UiId, never UiNode*

// Scene::enter or startup: UiTree is a World resource inserted by UiPlugin::init_world.
void build_hud(World &w) {
    UiTree &ui = w.resources().get<UiTree>();
    const UiId bar = ui.panel(0, Rect{4, 4, 200, 14}, UiLayout::Horizontal);
    w.resources().insert<Hud>(Hud{.score = ui.label(bar, "SCORE 0")});
    const UiId shop = ui.panel(0, Rect{4, 24, 120, 40}, UiLayout::Vertical);
    ui.focus(ui.button(shop, "drink"_id, "Drink potion"));
}

// Stage::Update: set_text only when the value changed (no per-frame churn, no allocation).
void hud_update(Ctx &ctx) {
    Hud &hud = ctx.local<Hud>();
    const Run &run = ctx.local<Run>();
    if (run.score == hud.shown) return;
    FixedString<32> text;
    format_to(text, "SCORE {}", run.score);
    ctx.local<UiTree>().set_text(hud.score, text.view());
    hud.shown = run.score;
}

// Stage::Update: UiEvent is presentation. Turn the click into an intent; never touch Run here.
void ui_clicks(Ctx &ctx) {
    for (const UiEvent &e : ctx.read<UiEvent>())
        if (e.kind == UiEvent::Kind::Clicked && e.id == "drink"_id)
            ctx.res<InputSystem>().queue_intent(Intent{.kind = "drink"_id});
}

// Stage::Input: the intent arrives in exactly one tick's InputFrame, and replays record it.
void apply_intents(Ctx &ctx) {
    Run &run = ctx.local<Run>();
    for (const Intent &i : input_frame(ctx).intent_list())
        if (i.kind == "drink"_id && run.potions > 0) --run.potions;
}
```

- `UiEvent` is emitted at Update -800, so a game Update system sees it on the next frame (a stage
  reads its own emissions one run later, [GUIDE.md](GUIDE.md#4-events)). Fine for clicks.
- Shorter: give the button an intent and skip `ui_clicks`. `ui.get(id)->intent = "drink"_id`
  (and `intent_a`) makes a Clicked, Changed or Submitted event queue
  `Intent{intent, intent_a, round(value)}` for you.
- `Intent{kind, a, b}` carries two `int64` payloads (item index, `Entity::bits()`). At most
  `kMaxIntents` (4) per tick; more are dropped and counted.
- An intent appears in exactly one tick's `InputFrame`, the next tick to run, and only in the
  focused world. If the rule cannot act on it that tick (a turn-based game between turns), copy it
  into reflected state (a pending field of a snapshot resource) or it is lost.
- Engine screens instead of a hand-built tree: `MenuScene(MenuSpec)` (buttons, toggles, sliders,
  choices, labels), `make_confirm_scene(question, yes_kind)`, `make_settings_scene()` (volumes,
  fullscreen, key rebinding), `make_text_scene(title, body)`. Their results come back through
  `Scene::resumed` (recipe 2.9).
- Other UI helpers: `ToastQueue::push(text)` (App resource, short banners), `MessageLog::add(text,
  color)` (roguelike log, World or App resource), `DialogueRunner` (`ui/dialogue.h`).
- For custom HUD drawing, push shapes and text into `RenderQueue` from a RenderUI system
  (recipe 2.15).

### 2.11 Saving and loading

```cpp
#include <optional>
#include <string>
#include <utility>
#include <lam/core/log.h>
#include <lam/input/driver.h>
#include <lam/scene/save.h>

constexpr int kSaveVersion = 1;

// App::data_dir(): the settings folder (user_data_dir(title), or $LAM_DATA_DIR), created on call.
// It is "" in scripted runs (--headless, --step, --hidden, --frames N, --bot) unless LAM_DATA_DIR
// is set, so bots and tests never touch real saves (or settings.json).
std::optional<SaveSlots> save_slots(const App &app) {
    std::string dir = app.data_dir();
    if (dir.empty()) return std::nullopt;
    return SaveSlots(std::move(dir), "hop", kSaveVersion, /*slot_count*/ 3);
}

// Every kSerialize component, every snapshot_resource, RNG streams, the tick and the exact
// entity layout, as one JSON document.
Status save_game(const App &app, const World &w, int slot) {
    std::optional<SaveSlots> slots = save_slots(app);
    if (!slots) return Status{};                                // headless: saving is a no-op
    LAM_TRY_ASSIGN(std::string data, serialize_world_text(w, app.serialize_context()));
    return slots->save(slot, SaveEnvelope{.game = "hop", .version = kSaveVersion,
                                          .summary_json = "{}", .data_json = std::move(data)});
}

// All or nothing: on any error the world is untouched. Call outside views (restore checks it).
Status load_game(const App &app, World &w, int slot) {
    std::optional<SaveSlots> slots = save_slots(app);
    if (!slots) return make_error(ErrorCode::NotFound, "no data folder (headless)");
    LAM_TRY_ASSIGN(SaveEnvelope env, slots->load(slot));
    return deserialize_world_text(w, env.data_json, app.serialize_context(), "save");
}

// Stage::Input: loading is a simulation change, so it comes in as an intent (F9 or a menu item).
void load_intent(Ctx &ctx) {
    for (const Intent &i : input_frame(ctx).intent_list())
        if (i.kind == "load"_id)
            if (Status s = load_game(ctx.app(), ctx.world(), 1); !s) LAM_LOG_WARN("load failed: {}", s.error().message);
}
```

- What is saved: components reflected with `kSerialize` (the default for `LAM_REFLECT`) and World
  resources registered with `snapshot_resource`. Unreflected components are not saved. Commands
  still pending are not saved.
- A loaded world continues bit-identically (same entity indices, RNG streams, tick). Restore hooks
  run afterwards and `WorldRestored` is emitted: rebuild caches there.
- `serialize_world_text` / `deserialize_world_text` wrap `serialize_world(w, DataWriter&, ctx)` /
  `deserialize_world(w, DataReader&, ctx)`, which write and read one JSON object
  (`{"layout":1,"tick":...,"seed":...,"entities":{...},"rng":[...],"pools":[...],"resources":[...]}`)
  at the writer's position, for a save that embeds the world in a bigger document. Unknown keys
  are errors. Asset fields are saved as paths and loads retain them in the current asset scope.
- `summary_json` (any JSON value) is what a slot list shows (`SaveSlots::summaries()`).
  `SaveSlots::latest()` returns the most recent slot (0 = none); there are also `exists`, `erase`
  and `slot_path`. Envelope migrations (`SaveMigration`) upgrade old versions one step at a time.
- Settings (volumes, bindings, your own keys) live in `SettingsStore` (App resource,
  `define_int/float/bool/string`, saved automatically); do not put them in saves.

### 2.12 Undo with `History`

```cpp
#include <cstdint>
#include <lam/ecs/snapshot.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_mover.h>

struct Moves { std::int32_t count = 0; };                       // sim counter: snapshot_resource it
LAM_REFLECT(Moves, count)
struct Undo { History history{8u << 20, 512}; };                // plain World resource: never snapshotted

// Scene::enter or startup.
void install_undo(World &w) {
    w.resources().insert<Moves>();
    w.snapshot_resource<Moves>();
    w.resources().insert<Undo>();
}

// Stage::Input. Snapshot only legal moves: decide first, push, then act. Never inside a view.
void step_or_undo(Ctx &ctx) {
    World &w = ctx.world();
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    History &history = ctx.local<Undo>().history;
    if (in.is_repeated(c.undo)) {
        history.undo(w);                                        // keeps the world tick by default
        return;
    }
    const Vec2 a = in.axis2(c.move);
    const IVec2 dir = a.x > 0.5f ? IVec2{1, 0} : a.x < -0.5f ? IVec2{-1, 0}
                    : a.y > 0.5f ? IVec2{0, 1} : a.y < -0.5f ? IVec2{0, -1} : IVec2{};
    if (dir == IVec2{}) return;
    Entity hero;
    for (auto [e, pos] : w.view<GridPosition>(With<Player>{})) hero = e;
    if (!hero || w.get<GridMover>(hero).moving) return;
    const IVec2 to = w.get<GridPosition>(hero).cell + dir;      // legality first, read-only
    if (ctx.local<TileWorld>().at(to) != TileShape::Empty || !ctx.local<GridIndex>().at(to).empty()) return;
    history.push(w);                                            // then snapshot, then act
    (void)try_step(w, hero, dir);
    ++ctx.local<Moves>().count;
}
```

- A snapshot covers every snapshotted pool (presentation components too: sprites and cameras
  rewind with the world), the entity table, RNG streams and snapshot resources. Keep `History`
  itself in an unsnapshotted resource, as above.
- Undo does not fire collision events or component hooks; restore hooks run and `WorldRestored`
  is emitted. Entity handles held by presentation resources may now point at dead or recycled
  entities: check `alive(e)` and a marker component before use.
- `History` is bounded by bytes and entries and reuses buffers (no allocation after warm-up).

### 2.13 Turn-based loop

```cpp
#include <lam/ecs/turns.h>
#include <lam/tilemap/grid_mover.h>

struct Monster {};
LAM_REFLECT_TAG(Monster)

// Scene::enter or startup: the scheduler is a snapshotted World resource. Actors carry
// TurnActor{.speed} (energy per round), GridPosition and GridMover; the player also has PlayerTurn.
void install_turns(World &w) { add_turn_scheduler(w); }

IVec2 dir_from(const InputFrame &in, AxisAction move) {
    const Vec2 a = in.axis2(move);
    if (a.x > 0.5f) return {1, 0};
    if (a.x < -0.5f) return {-1, 0};
    if (a.y > 0.5f) return {0, 1};
    if (a.y < -0.5f) return {0, -1};
    return {};
}

// Stage::Simulate. next_actor() returns null while grid movers animate (busy), when the per-tick
// budget is spent, or when nobody can act. The player's turn waits for input across ticks.
void take_turns(Ctx &ctx) {
    World &w = ctx.world();
    const InputFrame &in = input_frame(ctx);
    for (Entity e; (e = next_actor(w));) {
        if (w.has<PlayerTurn>(e)) {
            const IVec2 dir = dir_from(in, ctx.res<Controls>().move);
            if (dir == IVec2{}) break;                          // still the player's turn next tick
            Entity blocker;
            const StepResult r = try_step(w, e, dir, {}, &blocker);
            if (r == StepResult::BlockedEntity && w.has<Monster>(blocker)) ctx.commands().destroy(blocker);
            else if (r != StepResult::Moved) break;             // wall: no turn spent
            end_turn(w, e);
            break;                                              // one player action per tick
        }
        constexpr IVec2 kDirs[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
        (void)try_step(w, e, kDirs[ctx.rng("ai"_id).below(4)]);   // monsters: your AI here
        end_turn(w, e);
    }
}
```

- Order is deterministic: most energy first, ties by lower entity index. `end_turn(w, e, cost)`
  makes slow or fast actions.
- `grid_movers` (Animate -1200) tweens `Transform` between cells on tick time and keeps
  `TurnScheduler::busy` set, so the world waits for steps to finish.
- Movement keys held down: `ActionMap::set_repeat` + `InputFrame::is_repeated`.
- UI choices (use an item) arrive as intents; keep them in your run state if they land on a busy
  tick, as `examples/games/02_roguelike` does.
- `try_step` updates `GridPosition::cell` and `GridIndex` at once. An entity with a `GridMover`
  then tweens to the new cell centre; one without jumps there.

### 2.14 Bots with `InputDriver`, and headless runs

```cpp
#include <cstdio>
#include <span>
#include <lam/core/default_plugins.h>
#include <lam/input/driver.h>
#include <lam/physics/motors.h>
#include <lam/ui/menu_scenes.h>

// input_driver (TickBegin Once -1400) calls fill() every tick with the focused world (during a
// scene transition: the main world, ctx.focused() false; answer menus by repeating the intent
// until it is taken, and read ctx.app().world_stack() to see what is on screen). The frames
// arrive filled with this tick's live input AND the intents queued since the last tick (menu
// selections, UiNode clicks): edit them in place, never assign InputFrame{}. The writers produce
// what a real device would, so rules cannot tell the bot from a player, and --record-input /
// --replay still work.
class RunRightBot final : public InputDriver {
public:
    Controls controls;                                          // copied from the App resource
    void fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) override {
        InputFrame &f = frames[0];
        f.clear_devices();                                      // drop the human's buttons and axes, keep intents
        if (ctx.try_local<MenuState>() != nullptr) {            // an engine menu has focus: pick item 0
            (void)f.push_intent(Intent{.kind = kMenuSelectIntent, .a = 0});
            return;
        }
        f.set_axis(controls.move, {1, 0});
        for (auto [e, motor] : ctx.world().view<PlatformerMotor>(With<Player>{}))
            if (motor.on_ground && motor.on_wall_right) f.press(controls.jump);   // pressed + held this tick
    }
};

int main(int argc, char **argv) {
    RunRightBot bot;                                            // declared first: outlives the App
    App app({.title = "Hop"}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    // ... define controls, load prefabs, add systems as in the skeleton ...
    if (app.options().has("--bot")) {                           // unknown flags land in options().extra
        bot.controls = app.res<Controls>();
        app.res<InputSystem>().set_driver(&bot);                // not owned
    }
    const int code = app.run();
    if (app.options().headless)                                 // one machine-readable line for scripts
        std::printf("hop summary: ticks=%llu hash=%016llx\n", static_cast<unsigned long long>(app.tick()),
                    static_cast<unsigned long long>(app.main_world().state_hash()));
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
```

- `hop --headless --bot --frames 3000 --seed 7` runs on NullPlatform: no window or audio device,
  one tick per frame, synchronous asset loads. Two runs with the same seed must print the same
  hash; `--hash-every 600` logs intermediate hashes to diff.
- Keep the decision a pure function of world state so it can be tested without the App, like
  `bot_decide` in `examples/games/06_platformer/src/platformer/bot.cpp`.
- `InputFrame` writers: `press(a)` (went down this tick: pressed, held and repeated), `hold(a)`
  (still down on later ticks), `release(a)`, `set_axis(a, v)`, `push_intent(i)` (false when
  `kMaxIntents` are queued), `clear_devices()` (buttons, axes, pointer and wheel; keeps intents).
  `set_action_bit(bits, a, on)` is the free function under them.
- A driver sees only the focused world. Automating an unfocused world (a title demo under a menu)
  needs a tick system in that world.
- Worlds and resources live until `~App`, so after `run()` returns you can still read them: the
  main world's state with `app.main_world().resources().try_get<Run>()` (null if absent), App
  resources with `app.try_res<T>()`. With scenes the game world isn't the main world;
  `app.find_world("play"_id)` finds it (a scene's world is named after `Scene::name()`), as does
  `SceneStack::at(i)`. Or record the hash into an App resource from a TickEnd system.
- A bot drives engine menus through intents (`ui/menu_scenes.h`):
  `f.push_intent(Intent{.kind = kMenuSelectIntent, .a = index})` selects an item (`a` = index
  into `MenuSpec::items`, labels and disabled items included; `b` = the item's value) and
  `Intent{.kind = kMenuBackIntent}` backs out. The menu pops at the next tick boundary with the same
  `SceneMessage` a click gives, so the run replays.
- In tests, use `lam::test::HeadlessApp` (section 1.6) and `h.ticks(n)`. Add the plugins the
  rules need: `InputPlugin` (`lam/input/input_plugin.h`) for actions and drivers,
  `PhysicsPlugin` (`lam/physics/physics_plugin.h`) for motors and contacts, or
  `DefaultPlugins{.headless = true}` for everything.

### 2.15 Custom drawing and lights

```cpp
#include <lam/assets/asset_server.h>
#include <lam/core/fixed_string.h>
#include <lam/math/color.h>
#include <lam/render/font.h>
#include <lam/render/lighting.h>
#include <lam/render/render_queue.h>
#include <lam/render/shapes.h>
#include <lam/render/sprite.h>

struct Lamp {};                                                 // tag on lamp-post entities
LAM_REFLECT_TAG(Lamp)

// Scene::enter or startup: night lighting for this world. Lighting (World resource) is the switch;
// lights are presentation (frame time, never hashed), drawn at the interpolated Transform + offset.
void make_night(World &w, Entity hero) {
    w.resources().get<Lighting>() = Lighting{.enabled = true, .ambient = Color{40, 48, 90, 255}};
    w.add<Light2D>(hero, Light2D{.radius = 72, .flicker = 0.1f, .offset = {0, -10}});   // chest, not feet
}

// Stage::Render: world layers are world pixels drawn through the camera. Read positions with
// interpolated() so the drawing moves as smoothly as the sprites.
void draw_lamp_markers(Ctx &ctx) {
    RenderQueue &q = ctx.res<RenderQueue>();
    World &w = ctx.world();
    for (auto [e, tf] : w.view<Transform>(With<Lamp>{})) {
        const Vec2 p = interpolated(w, e, ctx.time().alpha).position;
        draw_triangle(q, p + Vec2{0, -20}, p + Vec2{-4, -28}, p + Vec2{4, -28}, Color{255, 220, 90, 255},
                      kLayerForeground);
    }
}

// Stage::RenderUI: screen layers (>= kFirstScreenLayer) are virtual pixels in painter's order.
void draw_hud(Ctx &ctx) {
    RenderQueue &q = ctx.res<RenderQueue>();
    q.rect(Rect{4, 4, 72, 11}, Color{0, 0, 0, 160}, kLayerUi);
    if (const Font *font = ctx.res<AssetServer>().get(ctx.res<RenderDefaults>().font)) {
        FixedString<32> text;
        format_to(text, "TICK {}", ctx.time().tick);
        draw_text(q, *font, text.view(), {6, 6}, TextStyle{.size = 8});   // built-in 6x8 font: 8 = 1x
    }
}
```

- `RenderQueue` (App resource) shape calls (`render/render_queue.h`):
  `rect(Rect r, Color c, std::uint8_t layer, float depth = 0)`,
  `rect_lines(Rect r, float thickness, Color c, std::uint8_t layer, float depth = 0)`,
  `line(Vec2 a, Vec2 b, float thickness, Color c, std::uint8_t layer, float depth = 0)`,
  `circle(Vec2 center, float radius, Color c, std::uint8_t layer, float depth = 0)`,
  `ellipse(Vec2 center, Vec2 radii, Color c, std::uint8_t layer, float depth = 0)` and
  `region(const TextureRegion &r, Vec2 position, Vec2 scale, float rotation, Vec2 pivot, Color tint, std::uint8_t layer, float depth, MaterialId material = 0, bool flip_x = false)`;
  `draw_triangle` (`render/shapes.h`) draws any winding as one quad and batches with them. Layers: `kLayerBackground` 0, `kLayerTiles` 16, `kLayerWorld` 64 (y-sorted
  actors), `kLayerForeground` 128, `kLayerLights` 190, `kLayerScreenFx` 200, `kLayerUi` 220,
  `kLayerOverlay` 240.
- A `Text` component draws a label at its entity (`Transform` + `Text::offset`) without a system.
  `TextStyle::size` is a pixel size: scale = size / `Font::base_size`.
- `Lighting::enabled` alone turns lights on for that world (the light target is made on first
  use). `Light2D::offset` hangs a light off the body (a lantern held up), or use a child entity.
- Lights multiply: the scene is scaled by `ambient` plus the lights, so a light can only give back
  a surface's own colour (a warm lamp on green grass reads green-yellow) and an ambient far from
  grey tints every grey thing. For a warm glow, add a glow `region` with `kMaterialAdditive` on
  `kLayerScreenFx`, which draws after the light composite (screen layers are virtual pixels:
  convert with `world_to_screen(ctx.local<CameraView>(), p)`); `examples/games/03_lantern_run`'s
  `draw_glows` does this.
- Draw from Render/RenderUI only: frame stages never write simulation state ([GUIDE.md](GUIDE.md#5-determinism-rules), rule 6).

---

## 3. Common mistakes

| Mistake | What happens | Do this instead |
|---|---|---|
| Calling `w.add`, `w.remove`, `w.destroy`, `PrefabLibrary::spawn`, `History::undo` or `deserialize_world` inside a `view` loop over an affected pool | `LAM_CHECK` abort in every build ("structural change during iteration") | Inside views use `ctx.commands()` (`spawn_prefab`, `add`, `remove`, `destroy`, `teleport`); run undo/load outside any view |
| Keeping a `T&` from `get<T>` across an add/remove of `T`, an `AssetServer::get` pointer past the stage, a `UiNode*` past the next `add`, a `read()` span past the system ([GUIDE.md](GUIDE.md#6-lifetimes)) | Dangling reads, random corruption | Store `Entity`, `AssetId<T>`, `UiId`, `Catalog` indices; look things up again |
| Calling `Audio`, `ParticlePools` or camera components from a tick system | Replays and the hash diverge; effects fire again after undo | Emit `PlaySound`, `BurstAt`, `ShakeAt` or a game event; presentation reacts in Update |
| Float time in rules (`timer -= ctx.time().frame_dt`) | Behaviour depends on frame rate; replays drift | Integer tick counters (`std::uint32_t cooldown_ticks`), `Lifetime{.ticks}`, `Fsm::ticks_in_state` |
| Letting `UiEvent` (or any frame-stage event) change rule state, or writing rule state from Update | Not replayable; differs with frame pacing | `InputSystem::queue_intent`, consumed from `input_frame(ctx).intent_list()` in a tick stage |
| `std::rand`, `<random>` devices, `time()`, `std::chrono` in rules, or `ctx.rng` in frame stages | Non-determinism; `ctx.rng` asserts outside tick stages | `ctx.rng("stream"_id)` in ticks (one stream per rule), `ctx.fx_rng()` for presentation |
| Forgetting `app.register_component<T>()` for a game component used in prefab JSON | Prefab load fails with an unknown component name when the reflecting header is not linked in | Register every game component and tag in your plugin's `build()`, before `PrefabLibrary::load` |
| `LAM_REFLECT` inside a namespace, in a `.cpp` only, before the type, or with an unqualified name | Compile errors, or two different reflections of one type | Global scope, right after the type, in its header, with the qualified name: `LAM_REFLECT(hop::Run, score)`; `LAM_REFLECT_ENUM` before the struct that uses the enum |
| `ctx.res<T>()` on a World resource or `ctx.local<T>()` on an App resource | Abort with a message naming the right call | App: `SettingsStore`, `AssetServer`, `ActionMap`, `SceneStack`, `PrefabLibrary`, `Audio`, `RenderQueue`, `ToastQueue`. World: `TileWorld`, `CameraView`, `Lighting`, `UiTree`, `ParticlePools`, `GridIndex`, `PathService`, `TurnScheduler`, `FovMemory` |
| Game rules registered with `app.add_system` once scenes exist, without a gate | They also run on menu/overlay worlds, where `ctx.local<Run>()` aborts | `.run_if = &has_local<Run>` on every game system, or `Scene::add_system` for scene-only systems |
| Emitting or reading an event no plugin registered (`BurstAt` in a headless test without `ParticlesPlugin`) | Abort (`event not registered`) | `app.add_event<T>()` in your plugin for every event it emits; it is idempotent, so registering `PlaySound`/`BurstAt` too is safe. For an optional plugin's event, `ctx.try_emit(e)` |
| Expecting an event emitted earlier in the same stage pass | The reader sees it one tick (or frame) later | Emit in an earlier stage, or accept the delay ([GUIDE.md](GUIDE.md#4-events)) |
| Reading a tag as a component: `view<PlatformerMotor, Player>()` | Does not compile (tags have no data) | `view<PlatformerMotor>(With<Player>{})` |
| Rule state in an unreflected component or a plain resource | Lost on load, not undone, not in the hash | `LAM_REFLECT` the component; `snapshot_resource<T>()` the resource (which also needs `LAM_REFLECT`) |
| Writing `ctx.commands()` with only `<lam/core/app.h>` | "incomplete type `Commands`" | `#include <lam/ecs/commands.h>` |
| `"jump"_id` with only `using namespace lam;` | Unknown literal operator | `using namespace lam::literals;` (or `str_id("jump")`) |
| `AppConfig` designated initialisers out of order (`{.virtual_size = ..., .title = ...}`) | Compile error | Declaration order: `title, window_size, virtual_size, tick_rate, ..., seed, ..., audio, ...` |
| Capturing lambdas as systems; `std::string` temporaries as system names | Does not compile / dangling name | Plain functions (captureless lambdas convert); names are string literals (static storage), unique per stage |
| Game system orders outside (-1000, 1000), or a Once system in Input..Animate | Runs between engine internals / `LAM_CHECK` abort | Keep orders inside the range; PerWorld for Input..Animate |
| `Fsm::set` then expecting `entered` on the new state's first run | With `set()` then `tick()` in one call, the next run sees `entered == false`, `ticks_in_state == 1` | Act on the transition where you call `set`, or test `ticks_in_state == 1` |
| A bot that assigns `frame = InputFrame{}` | Drops UI intents queued that tick (menus, restart) | `frame.clear_devices()`, then `press`/`hold`/`set_axis`; the intents stay |
| Choosing `Animator` clips or params in a game Animate system | The clip shows one tick late: the animators already ran at Animate -1400 | Choose clips, params and facings in Simulate or PostPhysics |
| Writing `kFlipX` on an animated sprite from a rule | The animators rewrite `kFlipX` every tick, so the flip is lost | Set `Animator::facing`; for a custom flip write it in Animate at order >= 0 or in Update |
| Ending a run with `run_if` alone | Motors keep applying the last intent, bodies keep their `Velocity`, animations keep playing | Push an overlay scene that blocks ticks (or `App::set_paused`); to stop only bodies zero motor intents and `Velocity` |
| `"sheet": "sprites/hero.json"` inside `sprites/hero.anim.json` (asset-root paths inside asset files) | Resolves to `sprites/sprites/hero.json`; the sheet and therefore the graph fail, and the hero never animates | Paths inside an asset file are relative to that file: `"sheet": "hero.json"` |
| Spawning a feet-anchored prefab from a Tiled rectangle object | It spawns at the rect centre, half a rect away from the feet | A point object placed at the feet, or the object property `"anchor": "bottom"` (positions table in [GUIDE.md](GUIDE.md#3-simulation-and-presentation)) |
| Naming a code-built asset in `prefabs.json` before adding it, or adding it unnamed | The prefab field is resolved as a file path: `PrefabLibrary::load` fails with "no loader for asset" (a name without a loadable extension), or a file load starts and fails; an unnamed runtime asset saves as `""` and loads back as none | `assets.add<T>(value, "fx/spark")` before `PrefabLibrary::load` |
| A driver destroyed before the App, or never reset | Use-after-free at shutdown | Declare it before the App, or call `set_driver(nullptr)` after `run()` |
| A moving platform without `Velocity` | A `MovingPlatform` without a `PathFollower` never moves, and dynamic bodies that touch a mover see zero speed | Give movers `Velocity` (+ `MovingPlatform`, `PathFollower`) |
| Teleporting bodies by writing `Transform` after a spatial query in the same tick | Later queries that tick can miss the moved bodies (the grids are rebuilt on the first query of a tick) | Query first, or call `refresh_queries(w)` after the move; queries are otherwise correct in any tick stage |
| Reflecting a ball once per `CollisionStarted` | Two contacts in one tick (wall, then paddle edge) undo each other, and a touch that continues never reports again, so the ball leaves the field | Sum the tick's normals first, then reflect once, only against motion into them (recipe 2.5, "A bouncing ball") |
| Swapping `CollisionStarted::a`/`b` and negating `normal` by hand | Easy to swap one and not the other | `other(c, self)` and `normal_from(c, self)` (`physics/events.h`) |
| An absolute box passed to `flee`, `box_path_clear` or `pick_wander_goal` | The box is offset twice: probes the wrong cells | Pass the box relative to the position arguments, `collider_box(collider, {0, 0})` |
| A feet-anchored walker on `PathRequest` + `follow_route` with zero offsets | The feet run through cell centres and the body scrapes the wall above in one-tile corridors | `PathRequest::start_offset = collider.offset` and `follow_route(route, tiles, position + collider.offset)` |
| A motor body spawned overlapping a solid tile | A platformer body never moves again and a top-down one can only walk out, and nothing is logged | Spawn clear of walls (one tile off the edge, or `nearest_free_spot`) |
| Assuming a pit kills the player | `TileWorld::outside_solid` makes off-map cells solid | Add a "fell out" rule below the last row, or open the bottom (`open_sides = kSideBottom`) |
| Gameplay in the main world, then pushing a pause or game-over menu | The first `push` takes the main world out of the world stack: it stops ticking, rendering and getting input until the scene stack is empty | Put gameplay in a `Scene`; keep main for startup only |
| Deriving `TileWorld` or caches from rule state and not rebuilding them after undo/load | Stale collision or AI data after a restore | `World::add_restore_hook(fn)` rebuilds it |

---

## 4. Example games

The rows follow the folders: first `examples/basics`, the skeleton and one demo per module in
learning order, then `examples/games`, numbered by size with the largest first. "Start reading"
paths are relative to the example's folder; each game's `README.md` has its controls and run
commands. The small games in `examples/arcade` are listed in its own README.

| Example | Pattern to copy | Start reading |
|---|---|---|
| `examples/basics/01_skeleton` | The skeleton of section 1, built with the engine so the documented API keeps compiling; its `assets/` has the prefab, a tiny `maps/level1.tmj` and a hero sheet and graph, so it also runs. | `main.cpp` |
| `examples/basics/02_math` | Console tour of lam_math: vectors, rects, geometry, `Rng` streams, easing and tweens; the same seed prints the same numbers. | `main.cpp` |
| `examples/basics/03_core` | Console tour of lam_core: strict `DataReader` errors with paths, `Catalog`, `DataWriter`, `SettingsStore`, allocation-free `FixedString` text, the fixed-step clock and event windows. | `main.cpp` |
| `examples/basics/04_ecs` | Console tour of lam_ecs: views, deferred `Commands`, a parent/child hierarchy, `History` undo/redo and `state_hash`. | `main.cpp` |
| `examples/basics/05_assets` | A custom asset type with its own loader and a dependency (`LoadContext::depend`), async loads (synchronous under `--headless`), `AssetEvent<T>` reactions (Loaded, Modified, Failed, Removed) and hot reload. | `main.cpp` |
| `examples/basics/06_input` | `ActionMap` actions with default bindings, `InputFrame` (tick) vs `FrameInput` (frame), rebinding by capture, an intent from frame code into the simulation, an `InputDriver` bot, record and replay to the same hash. | `main.cpp` |
| `examples/basics/07_render` | Y-sorted walkers from one sheet, a camera with look-ahead, dead zone and bounds, `ShakeAt` from the simulation, a label and a `draw_triangle` marker, render stats in a HUD line. | `main.cpp` |
| `examples/basics/08_physics` | A platformer hero on a hand-filled `TileWorld` (slopes, one-way ledge, ladder, spikes), dynamic crates and balls, a lift (`PathFollower` + `MovingPlatform`), a trigger flag, layers/masks/`solid_mask`, contact events read in PostPhysics with `other()`, F4 collider outlines. | `main.cpp` |
| `examples/basics/09_animation` | An Aseprite sheet driven by an `AnimGraph` (params, a trigger, `on_finish`), a frame event plus `current_slice` sword hitbox, tick `Tween` vs frame `FrameTween`. | `main.cpp` |
| `examples/basics/10_tilemap` | A Tiled dungeon with an embedded tileset (tile properties, an animated torch), objects spawned as prefabs and data-only objects, `PathRequest`/`PathRoute`, `try_step` with `GridMover` tweens, `update_fov` + `FogStyle`. | `main.cpp` |
| `examples/basics/11_particles` | A `.particles.json` torch textured from an atlas-packed PNG, `BurstAt` from a tick system on click, rain and snow presets; an attract mode exercises it under `--frames`. | `main.cpp` |
| `examples/basics/12_audio` | A sound bank with variants and pitch jitter, `PlaySound` from a tick system, a looping `AudioEmitter` orbiting an `AudioListener`, music crossfades, a ducking stinger, ambience and bus volumes. | `main.cpp` |
| `examples/basics/13_ui` | A main menu (buttons, slider, toggle, choice) beside a scrolling list in a `UiTree` themed by a `.theme.json` nine-slice, `UiEvent`s into toasts and a `MessageLog`, the engine settings scene. | `main.cpp` |
| `examples/basics/14_scene` | Scene stack push/pop/replace/reset with fades, saving the play world to a slot from a pause overlay and loading it back with a hash check, scripted tick systems via `Scene::add_system` (exit code 0 = round trip ok). | `main.cpp` |
| `examples/games/01_pathar` | Grid puzzle: pushes with `try_step` + `GridIndex`, decide-then-`History::push` undo, a snapshotted `PuzzleState` plus a restore hook that rebuilds `TileWorld`, the solver as an `InputDriver`, engine menu scenes (a `MenuScene` subclass, settings, text), progress in `SaveSlots` via `DataWriter`, `is_repeated` key repeat, autotiling into a runtime `TilemapAsset`. Bot run: the bot solves all 24 levels at par. | `src/sim/board.cpp`, `src/sim/sim_plugin.cpp`, `src/game/scenes.cpp` |
| `examples/games/02_roguelike` | Turn-based: `TurnActor`/`PlayerTurn` with `next_actor`/`end_turn`, `GridMover` busy waits, `try_step` bump-to-attack, `bsp_rooms`/`cellular_caves` carved with `set_tile`, `update_fov` + `FovMemory` + `FogStyle`, a shared `DijkstraMap` chase map, `Catalog<ItemDef>` from `parse_data`, `MessageLog`, use-item intents, `SaveSlots` + `serialize_world` with loading as an intent, a permadeath menu. Bot run: 3600 bot ticks reach depth 2 with 11 kills, and a mid-run save/load continues with the continuous run's `state_hash`. | `src/sim/turns.cpp`, `src/sim/dungeon.cpp`, `src/sim/save.cpp` |
| `examples/games/03_lantern_run` | Top-down chase: `TopDownMotor` player and wolves, `Fsm<WolfState>` reflected with `LAM_REFLECT_STRUCT`, `can_see`/`seek`/`hold_ring`/`pick_wander_goal`, `PathRequest` -> `PathRoute` + `follow_route` + `route_stale`, a hand-filled `TileWorld` from ASCII, night lighting (`Light2D` + `Lighting`, with additive glows drawn over the light pass), `FadeOccluder` canopies, camera look-ahead and shake from game events. Bot run: over 300 seeds the bot lights 24.3 lanterns a minute and loses 15 % of its first runs in the first minute; over 40 ten-minute runs it dies 242 times. | `src/sim/wolves.cpp`, `src/sim/player.cpp`, `src/view/view.cpp` |
| `examples/games/04_shooter` | Top-down arena: bullets via `Commands::spawn_prefab` with per-shot overrides and `Lifetime`, `ProjectileHit` damage, prefab `extends` for enemy variants, `DijkstraMap` + `FlowField` + `separation` crowds, mouse or right-stick aim, pause and game-over `MenuScene` overlays pushed from tick systems with restart as an intent, `UiTree` HUD and `ToastQueue`, one RNG stream per rule. Bot run: the bot clears waves headless (wave 4 and 148 kills in 3600 ticks with `--seed 7`), dies around wave 6-8 and restarts through the game-over menu; same seed, same hash in Debug and Release. | `src/shooter/rules.cpp`, `src/shooter/crowd.cpp`, `src/present/scenes.cpp` |
| `examples/games/05_apple_trail` | Snake: `GridPosition`/`GridMover` steps tweened by the engine, `GridIndex::at` self-collision, one snapshotted run resource, title/pause/over overlay scenes with `SceneStack::reset` per run, scene changes from tick systems, a BFS bot, the board drawn with `RenderQueue` shapes from `interpolated()`, `BurstAt` plus a game shake event. Bot run: over 864,000 ticks the bot averages 44.3 apples and 712 points per run. | `src/trail_rules.cpp`, `game/scenes.cpp`, `game/present.cpp` |
| `examples/games/06_platformer` (Hop Hill) | The skeleton grown into a level: rules plugin gated by `has_local<Level>`, Tiled map with objects spawned as prefabs, `PlatformerMotor` intents, coins/checkpoint/goal as `TriggerEntered` and stomps as `CollisionStarted` in PostPhysics, a `MovingPlatform` lift, `MotorAnimBridge` + `AnimGraph`, the level as a `Scene` with a `make_text_scene` result screen, a bot and a headless summary. Bot run: the bot reaches the goal in 522 ticks (4/12 coins, 1 stomp, no deaths) with the same hash in Debug, Release and record/replay. | `src/platformer/rules.cpp`, `main.cpp`, `src/platformer/bot.cpp` |
| `examples/games/07_breakout` | The smallest game, written from this file alone: paddle, ball and bricks drawn as shapes (no asset files), collision layers for walls, ball, paddle and bricks, rules and a bot in a pure `breakout_sim` library, a headless summary line. | `src/breakout/rules.cpp`, `main.cpp` |
