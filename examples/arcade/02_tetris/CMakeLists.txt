# Tetris. Rules and bot build into tetris_sim (no raylib), drawing lives in the executable.
lam_add_game(NAME tetris
  SOURCES src/main.cpp
  SIM_LIBRARY tetris_sim
  SIM_SOURCES src/tetris/rules.cpp src/tetris/bot.cpp
  SIM_DEPS lam_app lam_input lam_particles lam_render)
