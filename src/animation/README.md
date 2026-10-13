# lam_animation

Sprite sheets come straight from Aseprite's "Export Sprite Sheet" (array or hash JSON, trimmed
frames, frame tags in all four directions with repeat counts, slices), or from a bare `.png`. At
finalize each sheet's image is packed into its atlas group, so animated sprites batch with tiles
and other sprites. An `Animator` plays a sheet tag, or a clip of an `AnimGraph` (`.anim.json`), on
tick time, in integer milliticks, so a 100 ms frame at 60 Hz lasts exactly 6 ticks, with no
16/17 ms drift. Frame events (`AnimationEvent`), clip switches and `current_slice` hitboxes
replay bit-identically. `MotorAnimBridge` feeds graph params and the facing from physics
motors. `Tween` animates hashed state on tick time; `FrameTween` animates presentation
(`RenderOffset`, `Sprite`) on frame time and is never hashed. Nothing allocates per tick or per
frame.

## Usage

```cpp
app.add_plugins(DefaultPlugins{});                 // AssetsPlugin ... AnimationPlugin
AssetServer &assets = app.res<AssetServer>();
w.add<Sprite>(hero);                               // the animators system writes region + kFlipX
w.add<Animator>(hero, Animator{.graph = assets.load<AnimGraph>("sprites/hero.anim.json")});
w.add<AnimParams>(hero);                           // graph inputs (8 params, 4 triggers per tick)
w.add<MotorAnimBridge>(hero);                      // speed, vx, vy, grounded, moving, climbing, facing
fire_trigger(w.get<AnimParams>(hero), "attack"_id);                       // Simulate: switches this tick
for (const AnimationEvent &ev : ctx.read<AnimationEvent>()) {}            // Simulate, next tick
if (auto box = current_slice(anim, sheet, "hitbox"_id, flipped)) {}       // relative to the pivot
tween_to(ctx.commands(), door, {.to = {0, -32}, .ticks = 20, .relative = true, .tag = "open"_id});
```

Without a graph, `Animator{.sheet = id, .clip = "run"_id}` plays the sheet tag `run`, and
`play(animator, sheet, "jump"_id)` switches tags from game code; facing 1 (left, as from
`facing_index(dir, 2, current)`) flips the sprite. The `examples/basics/09_animation` demo shows a
graph-driven hero, a sword hitbox from a slice, and both tween kinds.

## Sprite sheets (`.json` Aseprite export, `.png`)

Export with Aseprite's "Array" or "Hash" JSON data, with Tags and Slices checked; trimming is
supported. Keys the engine does not read (`meta.app`, `layers`, `color`, ...) are ignored, so
exports load unchanged. Rules, each reported with the file name and JSON path:

- `meta.image` (a path next to the JSON file) and `meta.size` are required; the image must be
  exactly `meta.size`, at most 2048 on a side (`kMaxSheetImageSide`).
- Per frame: `frame` {x, y, w, h} inside the image; `filename` unique (array form; the hash key is
  the name); `rotated` must be false; `spriteSourceSize`/`sourceSize` describe trimming (the trimmed
  frame must fit its source size); `duration` in ms, rounded to whole ms, 1..65535.
- `frameTags`: `name` (unique), `from` <= `to` < frame count, `direction` (`forward`, `reverse`,
  `pingpong`, `pingpong_reverse`), `repeat` as a number or Aseprite's string (0 or absent = loop).
  Ping-pong does not show the end frames twice at a turn, and each sweep counts as one pass.
- `slices`: `name` (unique), `keys` with `frame`, `bounds` and an optional `pivot`; a key holds
  from its frame until the next key.
- Optional `"atlas": "<group>"` (top level or in `meta`) picks the atlas group (default
  `"default"`); `"none"` keeps a standalone texture. A sheet that does not fit a page, or an App
  without `AtlasSet`, also keeps its own texture.

A bare `.png` loads as a one-frame sheet (frame 0 = the whole image, 100 ms, no tags).

## Animation graphs (`.anim.json`)

Our own format, so reading is strict: unknown keys are errors.

```json
{"sheet": "hero.json", "start": "idle",
 "clips": [{"name": "idle", "tags": ["idle", "idle"]},
           {"name": "walk", "tags": ["walk_side", "walk_down", "", "walk_up"]},
           {"name": "attack", "tag": "attack_side", "mirror_left": true},
           {"name": "jump"}],
 "transitions": [{"from": "idle", "to": "walk", "param": "moving", "above": 0.5},
                 {"from": "walk", "to": "idle", "param": "moving", "below": 0.5},
                 {"from": "*", "to": "attack", "trigger": "attack"},
                 {"from": "attack", "to": "idle", "on_finish": true}],
 "events": [{"tag": "attack", "frame": 2, "name": "hit"}]}
```

- `sheet` (optional): the sheet, relative to the graph file. It is a load dependency, and
  finalize checks every clip tag and event frame against it. An `Animator` with no `sheet` uses
  the graph's sheet.
- `clips` (1..255, names unique): `tags` lists 1, 2, 4 or 8 sheet tags, one per facing; `tag` is
  shorthand for one tag; with neither, the clip plays the tag of its own name. `mirror_left`
  (default true): left-facing directions play the matching right-facing tag flipped on X, and a
  1-tag clip flips whenever the animator faces left. Set it false to use explicit left tags. An
  empty tag (`""`) falls back to the mirrored tag, else to the first tag.
- Facings run clockwise on screen from right (+y down): 2 = right, left; 4 = right, down, left,
  up; 8 = right, down-right, down, down-left, left, up-left, up, up-right. A graph's facing space
  is its largest clip `dir_count`, at least 2 (`AnimGraph::facing_dirs`), so a graph of 1-tag
  clips still faces left and flips; `facing_index(dir, n, current)`
  maps a direction into it, keeping `current` on ties and for a zero vector.
- `start`: the clip played while `Animator::clip` is empty (default: the first clip).
- `transitions`, tried in file order, first match wins. `from` is a clip, or `"*"`/`"any"`; `to`
  is a clip. Every condition given must hold: `param` above/below a threshold (`"above": x`,
  `"below": x`, or `"threshold"` with `"greater"`; a bare `param` means > 0.5); `on_finish`
  (the current tag reached its repeat count); `trigger` (fired this tick; the transition consumes
  it). A transition to the current clip is skipped unless it has a trigger, which restarts it.
  An any-clip transition needs at least one condition.
- `events`: `tag` names a sheet tag or a clip (any of its facing tags); `frame` is relative to
  the tag's `from`; `name` is the `AnimationEvent` name.

## Timing rules

- Each tick adds `round(1000 * speed)` milliticks (`speed` < 0, NaN or infinite = frozen); a frame
  advances when its time reaches `duration_ms * tick_rate`. At most `kMaxFrameStepsPerTick` (64)
  frames are stepped per tick; extra time is dropped.
- A clip starts on the tick it is requested and shows its first frame for its full duration
  (the start tick does not advance). Param and trigger transitions are evaluated before the
  tick's time is spent; `on_finish` switches happen on the tick the clip ends.
- A facing change within a clip keeps the position in the tag, the time on the frame and the
  pass count (the old engine's "a directional clip keeps its place").
- `AnimationEvent`s fire when a frame is entered, including the first frame of a started tag and
  frames entered by looping. They are emitted in Animate and read by Simulate on the next tick.
- Triggers last one animator tick: the animators system clears `AnimParams::triggers` after
  evaluating the graph, whether or not a transition used them.
- `current_slice` returns the slice key at the current frame relative to the sprite pivot (default
  (0.5, 1) of the untrimmed frame), mirrored about the pivot when `flip_x` (as the renderer
  mirrors a `kFlipX` sprite). Transform scale and rotation are not applied.
- Sheets and graphs are looked up per tick by `AssetId` (never cached across ticks), so hot
  reload needs no special handling: tags are re-resolved by name, and a tag whose index changed
  restarts.

## Tweens

`TweenSpec` steps wait `delay` ticks once, then play `repeat + 1` passes (255 = forever) of
`ticks` ticks; yoyo passes run backwards. The first tick of a pass already advances, so loops and
sequences never repeat a value. `relative` steps capture the target's value when the step starts
(Tint ignores `relative`; so does Alpha in a tick-time `Tween`, because `Sprite` is presentation
and never hashed). `Tween` (tick time) writes `Transform` (`Parent::local` for children) and
`Sprite`; `FrameTween` (frame time, `ticks x tick_dt` seconds) writes `RenderOffset` and `Sprite`,
and `frame_tween_to` adds a `RenderOffset` when needed (so does `frame_tweens` for a FrameTween
that comes from a save or a prefab without one). Both hold up to 4 steps and remove themselves
when done.
`TweenFinished{entity, tag}` fires for each tagged step and for the last step of a `Tween`.

## Components, assets, events, systems

| Kind | Name | Scope / flags | Notes |
|---|---|---|---|
| Component | `Animator` | S N H (tick time) | Sheet, optional graph, clip, playhead. Writes `Sprite::region` and `kFlipX`. |
| Component | `AnimParams` | S N H | 8 named float params (fixed slots) and 4 triggers per tick. |
| Component | `MotorAnimBridge` | tag, S N H | Params from `PlatformerMotor`/`TopDownMotor`/`Velocity`; written only while slots are free. |
| Component | `Tween` | S N H (tick time) | Up to 4 `TweenSpec` steps. |
| Component | `FrameTween` | S N (frame time, never hashed) | Up to 4 steps; presentation only. |
| Asset | `SpriteSheet` | `.json` (Aseprite), `.png` | Frames, tags, slices; `regions` atlas-mapped at finalize. |
| Asset | `AnimGraph` | `.anim.json` | Depends on its sheet; checked against it at finalize. |
| Event | `AnimationEvent` | World, capacity 1024 | Emitted Animate -1400; read by Simulate next tick (hitboxes) and by Update. |
| Event | `TweenFinished` | World, capacity 256 | Emitted Animate -1300; read by Simulate next tick. |
| System | `motor_anim_bridge` | Animate, PerWorld, -1500 | Params and `Animator::facing` for `MotorAnimBridge` entities. |
| System | `animators` | Animate, PerWorld, -1400 | Graph transitions, frame stepping, `AnimationEvent`s, trigger clearing, `Sprite` writes. |
| System | `tweens` | Animate, PerWorld, -1300 | Tick `Tween`s; `TweenFinished`; removes finished tweens (Commands). |
| System | `frame_tweens` | Update, PerWorld, -1500 | `FrameTween`s on `Time::frame_dt` into `RenderOffset`/`Sprite`. |

`AnimationPlugin` requires `lam.assets` and uses `AtlasSet` (from `RenderPlugin`) when present.
It registers no resources of its own. Benchmark: `lam_animation_bench` (label `bench`,
`-DLAM_BUILD_BENCH=ON`) ticks 10k graph-driven heroes with the bridge and tweens.
