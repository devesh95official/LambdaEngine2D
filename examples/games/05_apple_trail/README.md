# Apple Trail

A snake-style game. The traveller walks an orchard one tile at a time picking apples, and every
apple joins the trail behind. Golden apples are worth more but rot. The pace quickens every 4
apples, and bumping into a wall or your own trail ends the run.

Controls: arrows or WASD to turn (two quick turns are both remembered), Enter or Space to start
and play again, P or Esc to pause.

```sh
build/release/examples/games/05_apple_trail/apple_trail        # play
build/release/examples/games/05_apple_trail/apple_trail --bot  # watch the bot
build/release/examples/games/05_apple_trail/apple_trail --headless --bot --frames 3600 --seed 7
```

Rules are in `src/` (`apple_trail_sim`), scenes and drawing in `game/`.
