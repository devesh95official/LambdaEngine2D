# Grid Puzzle starter template

A small Sokoban-style game to copy, rename and grow: walk on a grid, push every crate onto a goal,
undo, restart, go to the next level.

The rules in `src/grid_puzzle/` build into `grid_puzzle_sim`, which links only the engine's pure
targets (no raylib), so they run the same in the window, under `--headless` and in the test. It
uses `GridPosition` + `GridMover` + `try_step` for moves and the glide, `History` for undo,
`snapshot_resource` so the counters are part of undo and the state hash, `ActionMap::set_repeat`
for held keys, a `UiTree` HUD and a camera. A push emits `PlaySound` and `BurstAt`; the rules never
call audio or particles directly. `SolverBot` is an `InputDriver` that solves each level with a
breadth-first search and presses the same buttons a player would.

```
CMakeLists.txt            the engine (add_subdirectory or FetchContent), the game, the test
main.cpp                  presentation and wiring: sprites, board, camera, HUD, effect assets, --bot
src/grid_puzzle/rules.h   types and the API of the rules (start reading here)
src/grid_puzzle/rules.cpp level parser, move rules, the tick system, RulesPlugin
src/grid_puzzle/bot.cpp   the solver and SolverBot (the --bot InputDriver)
tests/test_rules.cpp      the rules test; it also solves every level in assets/levels.txt
assets/levels.txt         the levels, in standard Sokoban text
assets/sprites/tiles.png  6 frames of 16x16: floor, wall, goal, crate, crate on goal, player
assets/sfx/push.wav       the push sound
tools/gen_assets.py       writes tiles.png and push.wav (Python standard library only)
```

## 1. Copy and rename

```bash
cp -R LambdaEngine2D/templates/grid-puzzle my_game && cd my_game
mv src/grid_puzzle src/my_game
grep -rlI --exclude-dir=build -e grid_puzzle -e GRID_PUZZLE -e 'Grid Puzzle' . \
  | xargs sed -i.bak -e 's/grid_puzzle/my_game/g' -e 's/GRID_PUZZLE/MY_GAME/g' -e 's/Grid Puzzle/My Game/g'
find . -name '*.bak' -delete
```

This renames the CMake `project()`, `lam_add_game` `NAME` and `SIM_LIBRARY`, `lam_add_test`, the
namespace and include folder, plugin and system names, the summary line and the window title.
`TODO(rename)` marks these places. The title (`AppConfig::title` in `main.cpp`) also names the
folder where settings and key bindings are saved, so pick the final name early.

## 2. Build

You need CMake 3.28+, Ninja and a C++20 compiler with `<format>` (Apple clang on macOS, clang or
GCC on Linux).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLAMBDA_ENGINE_DIR=/path/to/LambdaEngine2D
cmake --build build -j
```

- `LAMBDA_ENGINE_DIR` points at an engine checkout. The default (`../..`) only works while this
  folder is still at `templates/grid-puzzle` inside the engine; leave the flag out there.
- To have CMake download a pinned engine instead, replace the `add_subdirectory(...)` line in
  `CMakeLists.txt` with the commented `FetchContent_Declare` block above it. Pin a commit.
- Offline: add `-DLAM_DEPS_DIR=<folder with the raylib, json and doctest archives>`.
- Debug and RelWithDebInfo read `assets/` from this folder, so asset edits reload while the game
  runs. Release reads the copy next to the executable.

## 3. Run

```bash
build/grid_puzzle                                         # play
build/grid_puzzle --bot                                   # watch the solver play
build/grid_puzzle --headless --bot --frames 3000 --seed 7 # no window; prints one summary line
```

| Action | Keys | Pad |
|---|---|---|
| move | WASD or the arrow keys (hold to keep walking) | D-pad |
| undo | Z or Backspace (hold to rewind) | West |
| restart | R (undo brings the position back) | Select |
| next level, once solved | Enter or Space | South |

The headless run prints:

```
grid_puzzle summary: ticks=3000 level=3 cleared=50 moves=15 hash=<16 hex digits>
```

`cleared` counts solved levels, `level` and `moves` describe the level the run ended on, and `hash`
is `World::state_hash()`. Two runs with the same build, flags and inputs print the same hash; if
they don't, something in the rules isn't deterministic (engine `docs/GUIDE.md`, determinism rules).
`--record-input FILE` / `--replay FILE` record and replay a session, and `--print-schedule` lists
every system in run order.

## 4. Test

```bash
build/grid_puzzle_tests                       # or: ctest --test-dir build -L grid_puzzle
```

The test calls the pure rules directly (walking, pushing, blocked moves, parse errors, the solver)
and solves every level in `assets/levels.txt`, so an unsolvable level edit fails it. Use
`-L grid_puzzle` with ctest: the engine's own tests are registered but not built here, so a plain
`ctest` reports them missing. `-DGRID_PUZZLE_TESTS=OFF` skips the test.

## 5. How one move works

1. TickBegin: with `--bot`, `SolverBot::fill` presses a direction.
2. Simulate, `grid_puzzle.play`: handles undo, restart and next first, then applies the move to a
   copy of the position with `try_move`. Only a legal move goes on: `History::push` saves the
   World, then `try_step` moves the crate and the player (updating `GridIndex` and starting each
   `GridMover`). It updates `Run`, emits `PlaySound` and `BurstAt` on a push, and checks for a
   solved level.
3. Animate: the engine's `grid_movers` glides each `Transform` to its new cell over `kStepTicks`.
4. Update, `grid_puzzle.present`: adds a `Sprite` to pieces without one (after a level start,
   restart or undo), centres the camera and updates the HUD text when it changes. The engine plays
   the sound and spawns the particles.
5. Render, `grid_puzzle.board`: draws the floor, walls and goals under the sprites.

`Level`, `State`, `try_move`, `solved` and `solve` are plain functions on plain data, used by the
test and the bot. The ECS layer (`play`, `load_level`) asks them before it changes anything. A new
mechanic goes into `try_move` first, gets a test, and then shows up in the game.

## 6. Assets

- `assets/levels.txt` is standard Sokoban text: `#` wall, `@` player, `$` crate, `.` goal, `*`
  crate on a goal, `+` player on a goal. A blank line ends a level and `;` starts a comment. The
  list is read once at startup, so restart the game after editing it.
- `python3 tools/gen_assets.py` rewrites `assets/sprites/tiles.png` and `assets/sfx/push.wav` with
  the same bytes every time. The outputs are committed, so the build never needs Python. For real
  art, keep the frame order (the `Frame` enum in `main.cpp`) or change both together.
- The sound bank and the dust `EmitterConfig` are built in `load_effects` (`main.cpp`). If you move
  them to `.sounds.json` or `.particles.json` files, remember the loaders are strict (an unknown
  key fails the file) and paths inside an asset file are relative to that file, not to `assets/`
  (bank format in `src/audio/README.md`).

## 7. Grow it next

`grep -rn TODO .` lists the places to start, tagged `rename`, `rules`, `levels`, `controls`,
`feel`, `fx`, `art`, `ui`, `scenes`, `save/load` and `tests`. Common next steps (numbers are
sections of the engine's `docs/RECIPES.md`):

- New mechanics (ice, keys and doors, switches): extend `State` and `try_move` and add test cases.
  Reflect any new component and register it in `RulesPlugin::build`.
- Feel: on a blocked move, emit a bump `PlaySound` or a shake event and have Update turn it into
  `CameraShake::trauma` (2.3).
- Title screen, pause menu, level select (2.9, 2.10): each menu is a `Scene` with its own World.
  Game systems already use `run_if = &has_local<...>`, so they stay off menu worlds.
- Saves (2.11): `Run` is already saved. `TileWorld` is built from `Run::level`, so rebuild it in a
  `World::add_restore_hook` registered from `Plugin::init_world`.
- A hint key: call `solve()` from the current `read_state` and show or play the first step.
- Bigger boards: build a `Tilemap` and let the engine draw it instead of `draw_board` (2.8).
- A determinism test: drive a `lam::test::HeadlessApp` with `SolverBot` for a few thousand ticks
  and check that two runs give the same `state_hash` (1.6, 2.14).

## Pitfalls already handled

- Every game system has `run_if = &has_local<T>`, so a menu or overlay world can't hit a missing
  resource.
- `RulesPlugin` registers `PlaySound` and `BurstAt` itself, so the rules run in tests without the
  audio or particles plugins.
- Moves are checked on a copy before `History::push`, so undo only steps over legal moves. The
  camera is created before the first snapshot so undo can't remove it, and sprites are re-added
  after an undo.
- Walls are `Solid` with cost 0: `TileWorld::set` doesn't change the cost, and path finding needs
  cost 0 to treat a wall as blocked.
- The bot sets pressed, held and repeated for each button. It outlives the `App`, and
  `set_driver(nullptr)` runs before it is destroyed.
- The headless summary reads the main world after `run()` returns. Worlds still exist then, and
  this game has no scenes.
- Tick systems count ticks, tick stages use no randomness, and presentation only reads the
  simulation.
