# Top-down shooter starter template

A small arena shooter to copy and grow. Enemies walk in from spawn pads; you move, aim with the
mouse and shoot them for points. Three touches end the run and FIRE restarts it. It covers input,
motors, bullets, hits, score, game over and restart, plus a HUD, one sound, one particle burst, an
autoplay bot, a headless mode and a rules test.

```
CMakeLists.txt          standalone project: builds the engine as a subproject, then the game
src/game/game.h         the arena layout, components, Run resource, RulesPlugin, Bot
src/game/rules.cpp      the rules (top_down_shooter_sim: pure lam_* targets, no raylib)
src/game/bot.cpp        the autoplay InputDriver (--bot, tests)
src/main.cpp            the executable: drawn map, camera, HUD, effect assets, --bot, summary line
tests/test_rules.cpp    headless doctest: the bot scores, and the same seed gives the same hash
tools/gen_assets.py     writes every file in assets/ (python3 standard library only)
assets/                 prefabs.json, sprites/, maps/, audio/ (generated placeholders, committed)
```

| Stage, order | System | What it does |
|---|---|---|
| Input | `game.player_input` | input to motor and gun intent; FIRE restarts after game over |
| Simulate 0 | `game.tick_rules` | timers, enemies walk at the player, spawning speeds up with score |
| Simulate 10 | `game.fire` | spawns `bullet` prefabs through Commands |
| PostPhysics 0 | `game.bullet_hits` | `ProjectileHit` events: damage, score, then `PlaySound` and `BurstAt` events |
| PostPhysics 10 | `game.contact_damage` | `overlap_box` against enemies: hurt, invulnerability, game over |
| Update | `top_down_shooter.present` | HUD text, hurt blink, player faces the aim (presentation only) |

Every rule is gated with `.run_if = &has_local<Run>`, so a menu or pause scene added later won't
run them. Rule state lives in reflected components and the `Run` resource, which the state hash,
saves, undo and replays all cover.

## Copy and rename

1. Copy the folder anywhere, for example `cp -R templates/top-down-shooter ~/games/blaster`.
2. Rename the targets. Replace `top_down_shooter` with your name (for example `blaster`) in
   `CMakeLists.txt`, `tests/CMakeLists.txt` and `src/main.cpp` (system names and the summary line).
   Change the window title in `src/main.cpp`. The `game` namespace and the `src/game/` include
   folder can keep their names.
3. Outside the engine repo, point CMake at the engine: `-DLAMBDA_ENGINE_DIR=/path/to/LambdaEngine2D`
   (a checkout or a git submodule). The default, `../..`, only works inside `templates/`.
   To fetch the engine instead, swap the `add_subdirectory` line for the commented `FetchContent`
   block in `CMakeLists.txt` and pin `GIT_TAG` to a commit. Replays are only valid for the same
   build, so do not track a branch.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Inside the engine repo, `cmake -S templates/top-down-shooter -B build/tds ...` works the same way.
Offline, add `-DLAM_DEPS_DIR=<folder with the dependency archives>`. `-DGAME_BUILD_TESTS=OFF` skips
the test target and doctest.

Debug builds read `assets/` from this source folder, so saved edits hot-reload. Release builds use
the copy next to the executable.

## Run

```sh
build/top_down_shooter                     # play
build/top_down_shooter --bot               # watch the bot play
build/top_down_shooter --headless --bot --frames 3600 --seed 7
```

Controls: WASD or the left stick to move, the mouse to aim, left click, Space or RB to fire. FIRE
also restarts after game over. In dev builds, F3 shows the debug overlay and F4 the colliders.

`--headless` runs without a window or GPU, as fast as it can, and prints one line:

```
top_down_shooter summary: ticks=3600 score=... best=... kills=... deaths=... enemies=... hash=<16 hex>
```

Two runs with the same `--seed` must print the same hash. If they differ, something in the rules
reads the clock, a float timer, an unordered container or live input outside `input_frame(ctx)`
(engine `docs/GUIDE.md`, determinism rules). `--hash-every 600` logs hashes along the way to find
the first tick that differs. `--record-input FILE` and `--replay FILE` record and replay a session.

## Test

```sh
ctest --test-dir build -L top_down_shooter --output-on-failure
```

The test builds the same App as `--headless --bot` without the presentation, plays 40 seconds
twice with the same seed, and checks that the bot killed something and that both runs hash the
same. Add one `TEST_CASE` per rule you add.

## Assets

```sh
python3 tools/gen_assets.py
```

This rewrites everything in `assets/` except `prefabs.json`, with the same bytes every run. Re-run
it after editing the arena in `src/game/game.h` so the drawn map matches the walls. For real art,
replace the files but keep their names and Aseprite tags (`player`, `enemy`, `bullet`), or update
`assets/prefabs.json` and `src/main.cpp`.

Collision comes from `kArena` in `game.h`. The Tiled map is only drawn (`build_collision = false`),
so the art can't move a wall. Paths inside an asset file are relative to that file: `arena.tmj`
names `arena.tsj`, which names `tiles.png`. Paths in `prefabs.json` and in code are relative to
`assets/`. The sound bank and particle emitter are built in code (`load_fx` in `main.cpp`); if you
move them to `.sounds.json` or `.particles.json` files, follow their strict formats (engine
`docs/RECIPES.md` 1.5).

## Grow it next

Search for `TODO(` to find the places meant to change:

| Marker | Where | Idea |
|---|---|---|
| `TODO(level)` | `game.h` | draw your arena: `#` wall, `.` floor, `S` spawn pad, `P` player start |
| `TODO(tune)` | `rules.cpp` | invulnerability, enemy cap, safe spawn distance; speeds and HP are in `prefabs.json` |
| `TODO(enemies)` | `game.h`, `rules.cpp` | more prefabs (fast, tough, ranged), damage per kind, waves |
| `TODO(ai)` | `rules.cpp` | path around walls with `DijkstraMap` + `FlowField`, spread crowds with `separation` |
| `TODO(controls)` | `rules.cpp` | twin-stick aim on the right stick; rebinding (bindings persist in the settings file) |
| `TODO(feel)` | `rules.cpp` | a hurt event that the view turns into camera shake, flash and sound |
| `TODO(fx)` | `main.cpp` | more emitters and sound groups, made the same way as `pop` |
| `TODO(bot)` | `bot.cpp` | teach the bot each new rule, so `--headless --bot` keeps testing the whole game |
| `TODO(tests)` | `test_rules.cpp` | one case per rule |
| `TODO(build)` | `CMakeLists.txt` | new rule files go in `SIM_SOURCES`, new view files in `SOURCES` |

Beyond the markers:

- Menus, pause and game over: make the arena a `Scene` and push menu scenes over it. The rules
  already skip worlds without `Run`, but `run_if` only skips the game's systems; motors and
  velocities keep going, so a pause has to stop the world's ticks (an overlay whose
  `blocks_ticks()` is true).
- Pickups and powerups: trigger colliders on their own layer, handled from `TriggerEntered` in
  PostPhysics.
- Saves and undo already cover the rules' state, and the walls come back through a restore hook
  (`build_walls`). Never write saves under `--headless`, or a bot run would erase the player's save.

`examples/games/04_shooter` is a full game built the same way, with waves, flow-field crowds,
pickups, menus and a stress mode.

## Conventions to keep

- Rules read input only through `input_frame(ctx)` and the action ids in `Controls`. Live devices
  are never read in a tick.
- Timers count ticks as integers, and randomness comes from `ctx.rng("<rule name>"_id)`, one stream
  per rule.
- Rules never call `Audio` or the particle pools. They emit `PlaySound` and `BurstAt`, and register
  both events themselves, so the rules also run in tests without those plugins.
- Spatial queries (`overlap_box`) run in PostPhysics, after this tick's broadphase.
- Structural changes (spawn, destroy, teleport) go through `ctx.commands()`.
- The executable writes presentation only (UI, sprite tint and flip) and never creates or destroys
  entities from a frame stage.
- The bot is an `InputDriver`. It edits the frames in place, which keeps any queued UI intents, and
  sets the same action bits a player would.
