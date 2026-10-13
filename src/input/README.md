# lam_input

Named button and axis actions with per-player bindings saved to `SettingsStore`, a latched per-tick
`InputFrame` (each press reaches exactly one tick), the unlatched per-frame `FrameInput` for UI, key
repeat, gamepads, touch zones, text input, intents from UI to simulation, bots (`InputDriver`), and
input recording/replay.

```cpp
ActionMap &map = app.res<ActionMap>();                          // after app.add_plugins(...)
const AxisAction move = map.axis2("move", {{ActionMap::key(Key::W), ActionMap::key(Key::S),
    ActionMap::key(Key::A), ActionMap::key(Key::D)}}, {{PadAxis::LeftX, PadAxis::LeftY}});
const ButtonAction jump = map.button("jump", {ActionMap::key(Key::Space), ActionMap::pad(PadButton::South)});
map.set_repeat(jump, 20, 6);                                    // is_repeated(): press, +20 ticks, every 6
// Tick system (Stage::Input):    const InputFrame &in = input_frame(ctx);
//                                 in.axis2(move); in.is_pressed(jump); in.intent_list();
// Frame system (Stage::Update):  const FrameInput &fi = frame_input(ctx);  fi.is_pressed(jump);
//   UI -> simulation:            ctx.res<InputSystem>().queue_intent({"use"_id, slot});
// Rebinding UI:                  map.begin_capture(jump, /*player*/ 0, /*slot*/ 1);
```

Public headers: `lam/input/actions.h` (ActionMap, bindings, persistence), `lam/input/driver.h`
(InputSystem, InputDriver, InputRecorder, ReplayDriver), `lam/input/input_frame.h` (InputFrame,
FrameInput, ActionState), `lam/input/input_plugin.h`, `lam/input/keys.h` (device codes and
names), `lam/input/raw_input.h` (RawInput). Example: `examples/basics/06_input`.

## Resources, events and systems

| Kind | Name | Scope | Written by / when |
|---|---|---|---|
| Resource | `InputSystem` | App | `feed_raw` (FrameBegin -5000), input systems |
| Resource | `ActionMap` | App | game setup; capture/rebinding (cold); `load` at `finish` |
| Resource | `ActionState` (one `InputFrame` per player) | App | TickBegin -1500 (publish), -1400 (driver) |
| Resource | `FrameInput` (+ `PointerState`) | App | FrameBegin -4000 |
| Event (read) | `LifecycleChanged` (FocusLost/Background reset the latch) | App | platform |
| Components | none | | |

| Stage | Scope | Order | System | Target |
|---|---|---|---|---|
| FrameBegin | Once | -5000 | `raw_input_poll` (raylib devices -> `InputSystem::feed_raw`) | `lam_input_rl` (`RaylibInputPlugin`) |
| FrameBegin | Once | -4000 | `input_frame_update` (latch presses for ticks; `FrameInput` with `PointerState`; capture; settings mirror) | `lam_input` |
| TickBegin | Once | -1500 | `input_publish` (latch -> `ActionState`, intents) | `lam_input` |
| TickBegin | Once | -1400 | `input_driver` (`InputDriver::fill`, bots/replay) | `lam_input` |
| TickBegin | Once | -1300 | `input_record` (`InputRecorder::record`) | `lam_input` |

## Notes

- Presses and releases are OR-ed into a latch every frame and delivered to the next tick only.
  `held` and axes are the frame's state, shared by every tick of that frame. Tick code reads
  `input_frame(ctx)` (neutral for unfocused worlds); frame code reads `frame_input(ctx)`.
- The latch (not intents or held state) is cleared when `App::paused()` changes, when the focused
  world changes, and on `FocusLost`/`Background`, so the key that closed a menu doesn't also act in
  the game.
- Defaults bind player 0, who reads the keyboard/mouse and every pad (`kAnyPad`). Players 1..3 read
  the keyboard/mouse and no pad until `assign_player(p, pad, keyboard_mouse)`.
- `axis2` composites are normalised so diagonals have length 1, sticks use a radial deadzone, and the
  sum is clamped to length 1. `axis1` gives x in [-1, 1]. +y is down.
- Settings: `input.p<player>.<action>.<slot>` holds `"key:space"`, `"mouse:left"`,
  `"pad:south[@pad]"`, `"axis:+left_x[@pad][>threshold]"`, `"wheel:up"`, `"touch:<zone>"` or
  `"none"`. Axis slots hold `"up,down,left,right"` (axis2) or `"negative,positive"` (axis1).
  InputPlugin loads them at `finish`, applies saved values to actions defined later, and mirrors
  every change back into `SettingsStore` (the App writes the file on quit and on Background).
  Pad sticks are not saved.
- `--record-input FILE` records the frames each tick used (after any driver), starting at the first
  tick. `--replay FILE` installs a `ReplayDriver`, applies the recorded seed, and refuses files from
  another build or OS (unless `--replay-any-build`) or with a different tick rate.
- Frame and tick code doesn't allocate (fixed tables; the recorder writes into a reserved buffer).
  Defining actions, rebinding and settings sync are cold paths.
