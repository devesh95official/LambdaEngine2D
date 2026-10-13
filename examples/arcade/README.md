# Arcade

Ten small games drawn with engine shapes and the built-in font, no asset files. Each keeps its
rules and bot in a `<game>_sim` library and its drawing in `main.cpp`. The folders are numbered by
size, largest first.

| Game | What it is | Controls |
|---|---|---|
| [invaders](01_invaders) | Space Invaders with shields and a UFO | Left/Right move, Space fire, P pause |
| [tetris](02_tetris) | SRS rotation, hold, 7-bag | Arrows, Z/X rotate, Space drop, C hold |
| [frogger](03_frogger) | Cross the road and the river | Arrows or click to hop |
| [g2048](04_2048) | 2048 on a 4x4 board | Arrows or drag, Z undo, R new game |
| [minesweeper](05_minesweeper) | 9x9 or 16x16, safe first click | Left click reveal, right click flag |
| [dino](06_dino) | Endless runner, cacti and birds | Space jump, Down duck |
| [asteroids](07_asteroids) | Wrap-around rocks that split, in waves | Left/Right turn, Up thrust, Space fire |
| [missile](08_missile) | Missile Command, six cities | Mouse aim, click fire, 1-3 pick a base |
| [flappy](09_flappy) | Flap through the pipe gaps | Space, Up or click |
| [pong](10_pong) | First to 7, against the CPU or a friend | W/S or mouse; Up/Down for player 2 |

They build with the other examples:

```sh
cmake --preset release && cmake --build --preset release
build/release/examples/arcade/02_tetris/tetris                                 # play
build/release/examples/arcade/02_tetris/tetris --bot                           # watch the bot
build/release/examples/arcade/02_tetris/tetris --headless --bot --frames 3000  # summary line and state hash
```
