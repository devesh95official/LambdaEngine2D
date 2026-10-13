# Lantern Run

Gather the lanterns lit in a forest at night while wolves hunt you. Wolves see you only in a clear
line, path round trees with A*, circle while on cooldown and lunge; a dash gives a moment of
protection. Every 8 lanterns another wolf joins and the pack gets faster. Three bites end the run.
The forest is drawn with night lighting, canopies that fade as you walk under them, and camera shake.

Controls: WASD or arrows to move, Space or Shift to dash, Enter to start, P or Esc to pause.

```sh
build/release/examples/games/03_lantern_run/lantern_run        # play
build/release/examples/games/03_lantern_run/lantern_run --bot  # watch the bot
build/release/examples/games/03_lantern_run/lantern_run --headless --bot --frames 3600
```

Rules are in `src/sim/` (`lantern_run_sim`), presentation in `src/view/`.
