# Roguelike

A turn-based dungeon crawl. Each floor is carved procedurally into a Tiled template map (BSP rooms,
or cellular caves on every third floor). Explore with field of view and remembered fog, bump
monsters to attack, pick up potions and scrolls, level up and take the stairs down. Death is
permanent: the run's save is erased.

Controls: arrows, WASD or the numpad to move or attack (held keys repeat), Space to rest, `.`
or Enter to take the stairs, 1-8 or a click on the pack to use an item, F5 to save, F9 to load.

```sh
build/release/examples/games/02_roguelike/roguelike        # play
build/release/examples/games/02_roguelike/roguelike --bot  # watch the bot
build/release/examples/games/02_roguelike/roguelike --headless --bot --frames 6000 --seed 7
```

Rules are in `src/sim/` (`roguelike_sim`), scenes and the HUD in `src/game/`, and
`tools/gen_assets.py` regenerates the assets.
