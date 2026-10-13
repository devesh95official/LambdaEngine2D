# Shooter

Top-down arena survival. Waves of chasers (grunts, fast runners, big brutes) pour out of spawn pads,
steered by one shared flow field plus separation, growing from 16 to 300+ per wave. Shoot them,
grab drops (heart, rapid fire, spread shot) and survive. Death opens a game-over menu.

Controls: WASD or the left stick to move, the mouse to aim and click to fire, or arrow keys /
right stick to aim with auto-fire. Esc or Start pauses.

```sh
build/release/examples/games/04_shooter/shooter                 # play
build/release/examples/games/04_shooter/shooter --bot           # watch the bot
build/release/examples/games/04_shooter/shooter --headless --bot --frames 3600 --seed 7
build/release/examples/games/04_shooter/shooter --enemies 2000  # stress test
```

Rules are in `src/shooter/` (`shooter_sim`), presentation and scenes in `src/present/`, and
`tools/gen_assets.py` regenerates the assets.
