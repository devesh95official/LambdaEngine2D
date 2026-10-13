# Pathar

A Sokoban-like grid puzzle with water: push every stone onto a carved slab; a stone pushed into the
river sinks and fills it, so the water can then be crossed. 24 levels in 3 chapters, par and stars,
undo, restart, hints from a breadth-first solver, and saved progress.

Controls: WASD or arrows to walk and push (hold to keep walking), click a cell to walk there,
Z or Backspace to undo, R to restart, H for a hint, Esc to pause.

```sh
build/release/examples/games/01_pathar/pathar            # play
build/release/examples/games/01_pathar/pathar --level 9  # jump to a level
build/release/examples/games/01_pathar/pathar --bot      # the solver plays
build/release/examples/games/01_pathar/pathar --headless --bot --frames 3000
```

Rules are in `src/sim/` (`pathar_sim`), presentation, scenes and saves in `src/game/`.
