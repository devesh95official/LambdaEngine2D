# lam_scene

The scene stack and save files.

- Scenes: push, pop (with a typed message), replace and reset. Each scene owns one World and one
  asset scope. Operations are queued and applied at frame and tick boundaries; transitions are
  fades measured in ticks.
- Saves: a versioned JSON envelope with stepwise migrations, written to slot files atomically.
- World serialisation keeps the exact ECS layout, so a loaded run continues bit-identically.

Public headers:
- `lam/scene/scene.h`: `Scene`, `SceneMessage`, `Transition`, `SceneStack`, `ScenePlugin`
- `lam/scene/save.h`: `SaveEnvelope`, `write_save`/`read_save`, `SaveMigration`, `SaveSlots`,
  `serialize_world`/`deserialize_world` and their `_text` forms

Pure module: it links `lam_render` only for the `RenderQueue` push API that draws the fade. It
needs no raylib and runs headless.

## Usage

```cpp
class Level final : public lam::Scene {
public:
    explicit Level(std::string save = {}) : save_(std::move(save)) {}
    std::string_view name() const override { return "level"; }
    void enter(lam::App &app, lam::World &w) override {          // loads land in asset_scope()
        w.resources().insert<Score>();
        w.snapshot_resource<Score>();                             // saved, hashed, undone
        spawn_level(w);
        add_system(app, lam::Stage::Simulate, play, "level.play"); // runs on this world only
        if (!save_.empty()) (void)lam::deserialize_world_text(w, save_, app.serialize_context());
    }
private:
    std::string save_;
};
app.add_plugin<lam::ScenePlugin>();                               // part of DefaultPlugins
app.res<lam::SceneStack>().push(std::make_unique<Level>(),
                                {.kind = lam::TransitionKind::Fade, .ticks = 12});
// In a tick system: ctx.res<SceneStack>().push(std::make_unique<Pause>());  pause: pop(SceneMessage{...})
lam::SaveSlots slots(dir, "hop", /*version*/ 2);                  // <dir>/slot1.json ...
slots.save(1, {.summary_json = R"({"level":3})",
               .data_json = *lam::serialize_world_text(level->world(), app.serialize_context())});
lam::Result<lam::SaveEnvelope> s = slots.load(1, kMigrations);    // kMigrations[0] upgrades 1 -> 2
```

`examples/basics/14_scene` runs title → play → pause overlay (save) → play → pause overlay (load and
verify the hash) → title, unattended. Use `scene_demo --headless`, or
`scene_demo --hidden --frames N` for a windowed run.

## Components, resources, events and systems

| Kind | Name | Scope | Stage, order | Notes |
|---|---|---|---|---|
| Resource | `SceneStack` | App | written at FrameBegin/TickBegin -2000 | inserted by `ScenePlugin::build` |
| Event | `WorldRestored` | World | emitted by `deserialize_world` (through `World::restore`) | registered by lam_app |
| System | `scene_apply` | Once | FrameBegin -2000 | `SceneStack::apply`: drains the queue up to the next fade; while the App is paused it also advances a running fade |
| System | `scene_apply_tick` | Once | TickBegin -2000 | `SceneStack::apply_tick`: drains the queue, then advances a running fade by one tick |
| System | `scene_fade` | Once | RenderUI +1000 | one full-screen `RenderQueue::rect` at `kLayerOverlay`; only while the fade alpha is above 0 |
| System | per scene (`Scene::add_system`) | PerWorld (FrameEnd: Once) | any | `only_world` = the scene's world; removed with it |

`lam_scene` registers no components. Saves cover every `kSerialize` component and every
`snapshot_resource` registered with `kSerialize`.

## Behaviour

`push`/`pop`/`replace`/`reset` only queue. The queue runs in order at the next FrameBegin -2000
or TickBegin -2000, whichever comes first, so an operation issued by a tick system takes effect
before the next tick regardless of frame pacing. Operations queued by scene callbacks during an
apply run in the same apply.

`App::create_world(scene.name())` runs before `enter`, so every plugin's `init_world` has already
run on the new world. `App::destroy_world` runs after `exit`; it's deferred until the running
pass ends and the scene's systems go with the world.

The scenes become `WorldEntry`s, bottom first. A scene is:
- visible unless a non-`overlay()` scene is above it;
- ticking unless a `blocks_ticks()` scene is above it, or it's waiting for its assets;
- getting input unless a `blocks_input()` scene is above it, or a transition is running.

Scenes with no role are left out. At most `kMaxWorlds` worlds are listed; the bottom-most are
dropped with a warning. An empty stack gives `{main_world}`, and an App that never pushed a scene
keeps its own world stack.

A `Transition{.kind = Fade, .ticks = N}` fades out over N ticks, runs the operation at full
cover, holds until the new scene's assets are Ready (if `wait_for_assets`), then fades back in
over N ticks. Later operations wait for the fade-in to finish. `ticks == 0` is the same as `None`.

Each scene gets its own `AssetServer::begin_scope()`, which is current during `enter`. Scopes of
scenes that left the stack end after the new scene has entered, then `collect_garbage()` runs, so
an asset both scenes use is never reloaded on `replace`.

On App shutdown worlds go newest first, so scenes exit top first while their world still exists.
Queued scenes that never entered are destroyed without `enter` or `exit`.

### Saves

- `write_save` writes
  `{"format":"lam-save","game":G,"version":V,"summary":{..},"data":{..}}`. `read_save` also
  reads the old engine's `"lamda-save"`.
- The version must be in [1, current]. Migrations run one step at a time; a step that leaves a
  key unread fails instead of dropping data.
- Slots are `<dir>/slot<n>.json` with n in [1, count], written with `write_file_atomic`.

### World format

- The entity table is stored as is: generations, states and the free list.
- RNG streams, pools in name order (entities as raw `[index, generation]`, values through
  reflection) and `kSerialize` snapshot resources are stored.
- Loading builds a `WorldSnapshot` and `validate()`s it before touching the world, then calls
  `World::clear()` and `World::restore()`. A failed load changes nothing.
- Resources only load into types the world already holds, so insert them in `enter()` before
  loading.

## Limits

- Events still in flight at the save point (emitted in tick N for readers in tick N+1) are not
  saved.
