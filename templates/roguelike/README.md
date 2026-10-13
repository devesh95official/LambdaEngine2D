# Roguelike starter template

A small turn-based dungeon crawler to copy and grow. The core loop works end to end:

- Each floor is generated from `(seed, depth)`: BSP rooms joined by corridors.
- Field of view with fog: unexplored cells are black, remembered cells dimmed, and monsters are
  drawn only while the hero can see them.
- Energy turns (`TurnScheduler`). Monsters move while the hero's step animates, and the next turn
  waits for every step to finish.
- Bump a monster to attack it. Monsters sleep until they see you, then chase and bite.
- The gold stairs take you one floor deeper (more and tougher monsters, more score).
- After death, Space starts a new run.
- A two-line HUD, plus a spark burst, a hit sound and camera shake on every hit. The rules only
  emit events; presentation turns them into effects.
- A bot (`--bot`), a headless summary line, and a doctest that checks the rules replay
  bit-identically.

About 520 lines of C++ plus a 45-line test. `TODO(...)` marks the usual places to extend it.

## Files

```
CMakeLists.txt          standalone project: engine subproject, lam_add_game, the test harness
src/main.cpp            presentation: camera, fog drawing, HUD text, effects, --bot, summary line
src/game/rules.h        components, RunState, the Hit event, RulesPlugin, BotDriver (reflected)
src/game/rules.cpp      the rules: floors, turns, bump attacks, stairs, death, the restore hook
src/game/bot.cpp        the bot: an InputDriver that presses the same keys a player would
tests/test_rules.cpp    headless rule test (the bot plays; same seed = same state hash)
assets/prefabs.json     "player" and "monster" (shared "actor" base)
assets/sprites/         actors.png + actors.json (Aseprite sheet; frame tags = Animator clips)
assets/sfx/             hit.wav + game.sounds.json (the "hit" sound group)
tools/gen_assets.py     regenerates the placeholder PNG, sheet and WAV (Python standard library)
```

`roguelike_sim` (everything in `src/game/`) links only pure engine targets, never raylib, so the
test and `--headless` runs need no window, GPU or audio device.

## Copy and rename

```bash
cp -R LambdaEngine2D/templates/roguelike my_game
grep -rn -i "roguelike" my_game --exclude-dir=assets
```

Rename:
- `project(roguelike)` and `lam_add_game(NAME roguelike ... SIM_LIBRARY roguelike_sim)` in
  `CMakeLists.txt`.
- `lam_add_test(MODULE roguelike ...)` and `lam_roguelike_tests` in `tests/CMakeLists.txt`.
- `.title = "Roguelike"` in `src/main.cpp`. The title also names the folder that holds settings
  and saves. Rename the `"roguelike summary:"` prefix and the log prefix there too.
- Optionally the C++ namespace `game` and the plugin name `"game.rules"`.

## Build

Inside the engine repository (`templates/roguelike`) the defaults find the engine:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

After copying the folder elsewhere, point it at an engine checkout or a git submodule:

```bash
cmake -S my_game -B my_game/build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DLAMBDA_ENGINE_DIR=/path/to/LambdaEngine2D
```

- Offline: add `-DLAM_DEPS_DIR=<folder with the dependency archives>`.
- To fetch the engine instead of using a checkout, use the commented FetchContent block in
  `CMakeLists.txt` in place of the `add_subdirectory` line. Pin a commit hash, since replays and
  hashes are only valid for one build. To use a local engine with that block, pass
  `-DFETCHCONTENT_SOURCE_DIR_LAMBDA_ENGINE=/path/to/LambdaEngine2D`.
- `-DGAME_BUILD_TESTS=OFF` skips the test. The engine's own tests and examples are never built here.
- Non-Release builds read `assets/` from the source folder (`build/roguelike.assetroot`), so asset
  edits hot-reload in a windowed run. Debug builds also have the F3 dev overlay. Release uses the
  copy next to the executable.

## Run

```bash
./build/roguelike                                    # arrows/WASD/numpad: move or attack; Space: wait
./build/roguelike --bot                              # watch the bot play
./build/roguelike --headless --bot --frames 3000 --seed 7
```

The headless run prints one line, for example:

```
roguelike summary: ticks=3000 depth=2 kills=1 score=120 hp=24 dead=0 turns=227 hash=bb17cfc425a0ce13
```

Values differ between builds and platforms. Within one build, two runs with the same `--seed` must
print the same `hash`; if not, something in the rules isn't deterministic. `--hash-every 100` logs
intermediate hashes to find the first tick that differs, and `--record-input FILE` /
`--replay FILE` record and replay a session. `--seed` picks the dungeon. The fields describe the
current run; the bot starts a new one whenever it dies.

## Test

```bash
ctest --test-dir build --output-on-failure            # or ./build/tests/lam_roguelike_tests
```

The test plays 1800 ticks with the bot twice on the same seed. It checks that both runs end with
the same state hash and that turns passed and the hero killed a monster or took the stairs. Add a
`TEST_CASE` for each rule you add.

## Assets

`python3 tools/gen_assets.py` rewrites `actors.png`, `actors.json` and `hit.wav`, byte-identical on
every run. Commit its outputs so the build never needs Python. `prefabs.json` and
`game.sounds.json` are written by hand. Format rules:

- Paths inside an asset file are relative to that file: the sheet's `meta.image` is `"actors.png"`
  and the bank's `"file"` is `"hit.wav"`, not `sprites/...` or `sfx/...`. Paths in code and in
  `prefabs.json` are relative to `assets/` (`"sheet": "sprites/actors.json"`).
- `.sounds.json` is strict: `{"bus": "sfx", "groups": {"<id>": {"file": "x.wav", ...}}}`, with
  `groups` an object keyed by group id. `.anim.json` graphs are strict too (see the animation
  module README). An unknown key is a load error.
- `PrefabLibrary::load` is all-or-nothing: one bad field, such as an unloadable sheet, fails the
  whole file and the game exits with the error. Load prefabs after `DefaultPlugins`.
- For real art, any Aseprite export works (File > Export Sprite Sheet, JSON array, tags on). Keep
  the tag names the prefabs use as clips.

## Grow it next

`grep -rn "TODO(" src tests tools` lists the hooks. A sensible order:

1. More monsters: prefabs that `"extends": "monster"` with other `Stats`, a glyph each in
   `gen_assets.py`, and a spawn table by depth in `build_floor`. For smarter AI, use a shared
   `DijkstraMap` toward the hero (as the bot does toward the stairs), an `Fsm<State>`, and
   `ctx.rng("ai"_id)` for wandering.
2. Items: `FloorLocal` entities with `GridPosition{.blocks = false}`, a "pick up" action in
   `Controls`, and the pack in a reflected component.
3. A message log: keep the lines in reflected rule state (like `RunState::message`) and copy them
   into the UI in Update. A UI-only log isn't saved or undone.
4. Tiles: draw a tileset instead of flat rects with a `Tilemap` that has `build_collision = false`
   (engine `docs/RECIPES.md` 2.8). The rules keep owning `TileWorld` and the restore hook rebuilds it.
5. Menus and game over (`docs/RECIPES.md` 2.9). Move gameplay into a `Scene` first: once any scene
   is pushed, the main world stops ticking and drawing. Keep `run_if = &has_local<game::RunState>`
   on every game system, since menu worlds run the same systems.
6. Saves (`docs/RECIPES.md` 2.11). Everything the rules own is already reflected. Headless and bot
   runs must never write or erase the player's save.

To keep it deterministic and testable:
- Rules go in `src/game/` and run in tick stages only. Count time in turns or ticks and take
  randomness from `ctx.rng(stream)`.
- Rules emit events (`Hit`, `PlaySound`) and never call audio, particles or the camera. Register
  every event you emit in `RulesPlugin::build` (`add_event` is idempotent).
- Frame stages (Update, Render) read rule state and write only presentation: sprites, camera, UI
  text, the render queue. They never create or destroy entities in the game world. UI that changes
  the game queues an intent (`docs/RECIPES.md` 2.10).
- Never put `lam_engine` or a raylib backend in `SIM_DEPS`.
