# lam_ui

Retained UI, one `UiTree` per world. It has panels, labels, buttons, toggles, sliders, choices,
text fields, images, scrolling lists and grids, and wrapped text blocks; anchor/pivot placement,
stacks and grids; keyboard and gamepad focus navigation (the old MenuModel rules) plus pointer
hover, click and drag; and a data-driven `UiTheme` with fonts and an atlas nine-slice.

`UiTree` is drawn through `RenderQueue` at screen layers. UI text uses fixed buffers and layout
caches its measurements, so the per-frame path does not allocate. `UiEvent` is presentation:
anything that changes simulation goes through `InputSystem::queue_intent`. The module also
provides toasts, a roguelike message log, branching dialogue, and the engine menu, confirm,
settings and text scenes.

## Usage

```cpp
app.add_plugins(DefaultPlugins{});                               // ..., ScenePlugin, UiPlugin
UiTree &ui = app.main_world().resources().get<UiTree>();         // one per world (init_world)
ui.set_theme(app.res<AssetServer>().load<UiTheme>("ui/game.theme.json"));
UiId menu = ui.panel(0, {});                                     // auto-sized vertical stack
ui.get(menu)->anchor = ui.get(menu)->pivot = {0.5f, 0.5f};       // centred on the virtual screen
UiId play = ui.button(menu, "play"_id, "Play");
ui.slider(menu, "volume"_id, "Volume", 0.8f, 0, 1);
ui.get(ui.button(menu, "buy"_id, "Buy potion"))->intent = "shop.buy"_id;   // reaches the next tick
ui.focus(play);
// Update system after ui_update (-800): react to this frame's events (presentation only).
for (const UiEvent &e : ctx.local<UiTree>().events()) if (e.id == "play"_id) show_level_select(ctx);
// Menus as scenes: the choice comes back in Scene::resumed as a SceneMessage.
app.res<SceneStack>().push(make_confirm_scene("Quit to title?", "quit"_id));
```

Builders return `UiId`s. Keep the ids rather than `UiNode*`, since pointers are only valid until
the next `add` or `remove`.

`tree.events()` holds this frame's events for systems that run after `ui_update`;
`ctx.read<UiEvent>()` delivers the same events one frame later through the world's event window.

To change simulation state, set `UiNode::intent` (or call `InputSystem::queue_intent`) and read
`input_frame(ctx).intent_list()` in a tick system.

`.theme.json` (strict: unknown keys are errors; paths are relative to the theme file):

```json
{"font": "ui.fnt", "font_size": 8, "text": "#f0f0f0", "panel": [40, 44, 64, 235],
 "button": [64, 70, 100], "button_focus": [104, 116, 190], "button_down": [30, 32, 48],
 "nine_slice": {"image": "panel.png", "rect": [0, 0, 12, 12], "border": 3}, "atlas": "default",
 "click_sound": "ui_click", "focus_sound": "ui_move",
 "classes": {"title": {"text": "#ffd75e", "background": false, "align": "center", "font_size": 16}}}
```

`.dialogue.json`:

```json
{"lines": [{"id": "hi", "speaker": "Shopkeeper", "text": "Hello there!"},
           {"id": "buy", "text": "Buy a potion?", "choices": [{"text": "Yes", "next": "yes"}, {"text": "No"}]},
           {"id": "yes", "text": "Thanks!", "end": true}]}
```

## Components, resources, events, systems

| Kind | Name | Scope / stage | Notes |
|---|---|---|---|
| Resource | `UiTree` | World (`UiPlugin::init_world`) | Widgets, focus, layout results and this frame's events. Not snapshotted or hashed (presentation). |
| Resource | `UiNav` | App | The `ui.up/down/left/right/confirm/back/erase` actions: arrows/WASD + d-pad/left stick, Enter/Space + South, Escape + East, Backspace. Directions and erase repeat (18 ticks, then every 4). |
| Resource | `ToastQueue` | App | Fixed capacity (8); duplicates merge into "text xN". |
| Resource | `MenuState` | World of a menu scene | The spec, item widgets, hooks and the closing message (`result`). |
| Resource | `MessageLog` | World or App (the game inserts it) | A ring of 96-byte lines. |
| Asset | `UiTheme` | `.theme.json` | Font and nine-slice image are dependencies; the nine-slice is packed into the theme's atlas group. |
| Asset | `DialogueScript` | `.dialogue.json` | Strict parser; `DialogueRunner` plays it. |
| Event | `UiEvent` | World; emitted Update -800 | Clicked, Changed, Focused, Back, Submitted. Read in frame stages only. |
| Intent | `UiNode::intent`, `menu.select`, `menu.back` | queued at frame time | Delivered in the next tick's `InputFrame` and recorded. |
| System | `ui_update` | Update, PerWorld, -800 (`run_if` UiTree) | Clears events, lays out, applies `FrameInput` (neutral unless the world is focused; ignored while an ActionMap capture runs), lays out again if needed, emits `UiEvent`s, plays the theme's click/focus sounds on the "ui" bus, and queues node intents. |
| System | `toasts_update` | Update, Once, +500 | `ToastQueue::update(frame_dt)`. |
| System | `draw_ui` | RenderUI, PerWorld, -1000 (`run_if` UiTree) | Re-lays out a tree that was edited after `ui_update`, then `draw_ui_tree` at `kLayerUi`. |
| System | `draw_toasts` | RenderUI, Once, +500 | `draw_toast_list` at `kLayerOverlay`, bottom centre. |
| System | `ui.menu_frame` | Update, PerWorld, -700, `only_world` = the menu's | Menu events → intents; settings hooks (values, rebinding). |
| System | `ui.menu_tick` | Input, PerWorld, -1000, `only_world` = the menu's | Intent → `SceneMessage` → `SceneStack::pop` at the tick boundary. |

## Behaviour notes

Layout is two passes (measure, then arrange) over the whole tree in every `ui_update`. Labels
re-measure only when their text, font or scale changes, and text blocks re-wrap only when their
text, width or font changes (`stats()` counts both). A `rect` size of 0 means the natural size,
`fill` stretches a node to its parent's content box and `max_size` caps it. Positions are floored
to whole virtual pixels. Without a font, layout uses 6×8 metrics, so headless tests lay out the
same as with the default font.

Lists and grids clip their content and scroll by whole rows; `scroll` is the first visible child.
Focus and the mouse wheel both scroll items into view. Children that are scrolled out are neither
drawn nor hit.

Navigation:
- Labels and disabled or hidden nodes are skipped, and focus wraps inside the menu (the outermost
  vertical stack or grid).
- Rows and grids take left/right.
- Value widgets take left/right: a slider steps on its grid without drift, a choice cycles.
- Confirm clicks a button, flips a toggle, steps a choice, or submits a text field and moves on.
- `set_focus_root` confines input to a subtree, for modal popups.

Pointer:
- Hover only changes focus when the pointer moves.
- Pressing and releasing on the same node clicks it once.
- Sliders follow the drag, and clicking the left half of a choice steps it back.
- The wheel scrolls the list under the pointer.

One action produces at most one event, and a tree emits at most 64 events per frame (extras are
counted in `stats().dropped_events`).

Menus replay exactly. The intent carries the item index and its payload (a button's value, or a
text field's interned text), and the tick turns that into the message without reading any
frame-time widget state. Value items edit the SettingsStore key named by their id (create the id
with `intern`). The settings scene edits the `audio.<bus>.volume` keys (and `Audio`),
`display.fullscreen` (applied by the platform), `debug.show_fps` (LAM_DEV) and key bindings
(capture, then `ActionMap::save`).

Toasts last at least 0.6 s, fade in over 0.15 s and out over the last 0.4 s. A duplicate restarts
the toast that's already showing.

`DialogueRunner` copies its script at `start()` and types by UTF-8 code point. `advance()`
finishes the typing first, then moves to the next line; choices wait for `choose()`.
`draw_dialogue_box` caches the wrap in a `TextLayout` the caller keeps.

`ui_update`, `draw_ui`, `toasts_update`, `draw_toasts` and the draw helpers don't allocate in
steady state. Allocation only happens when the tree grows, `TextBlock` text is set, or a text field
submits (one `intern`).

## Files

| File | Contents |
|---|---|
| `include/lam/ui/ui.h` | `UiTree`, `UiNode`, `UiEvent`, `UiTheme` (+ classes, parser, loader), `UiInput`, `UiNav` |
| `include/lam/ui/ui_draw.h` | `draw_ui_tree`, `draw_nine_slice`, `draw_toast_list`, `draw_message_log`, `draw_dialogue_box` |
| `include/lam/ui/menu_scenes.h` | `MenuScene`, confirm/settings/text factories, `build_menu` and the intent/message functions |
| `include/lam/ui/toasts.h`, `message_log.h`, `dialogue.h`, `ui_plugin.h` | as named |
| `ui_tree.cpp`, `ui_layout.cpp`, `ui_input.cpp`, `ui_draw.cpp`, `detail/ui_tree_impl.h` | tree storage, layout and hit testing, navigation and pointer, drawing |
| `ui_theme.cpp`, `toasts.cpp`, `message_log.cpp`, `dialogue.cpp`, `menu_scenes.cpp`, `ui_plugin.cpp` | the rest |

## Tests and example

`tests/ui` (`lam_ui_tests`) has ports of the old menu_model, toasts and dialogue tests; tree,
layout, navigation (rows, grids, side-by-side columns, lists, text fields), pointer and drawing
tests; the theme and dialogue parsers and loaders; the plugin's system slots, focus, event
delivery, determinism and a printed frame-cost budget; menu payload rules, settings writes and the
confirm and text scenes; and a `CHECK_NO_ALLOC` run over 600 frames.

`examples/basics/13_ui` is a menu with buttons, a slider, a toggle and a choice next to a scrolling
level list, plus toasts, a message log and an attract mode. Run it with
`ui_demo [--frames N] [--hidden] [--headless]`; `python3 make_assets.py` regenerates the
nine-slice PNG.
