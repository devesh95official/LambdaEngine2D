# Platformer ("Hop Hill")

A two-screen side-scroller on a Tiled map: slopes, one-way planks, a ladder, spike pits, a moving
lift, 12 coins, a slime you can stomp, a checkpoint and a goal flag. You have three lives; the HUD
shows coins, lives and score.

Controls: arrows or WASD to move, Space or Z to jump (hold for higher), Down to drop through
planks or climb. Gamepad: left stick and South.

```sh
build/release/examples/games/06_platformer/platformer                                 # play
build/release/examples/games/06_platformer/platformer --bot                           # watch the bot
build/release/examples/games/06_platformer/platformer --headless --bot --frames 3600  # summary + hash
```

Rules, level spawning and the bot are in `src/platformer/` (the pure `platformer_sim` library);
`main.cpp` holds scenes, camera, HUD and effects. `tools/gen_assets.py` regenerates the assets.
