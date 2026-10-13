# Minesweeper. Rules and bot build into minesweeper_sim (no raylib), drawing lives in the executable.
lam_add_game(NAME minesweeper
  SOURCES main.cpp
  SIM_LIBRARY minesweeper_sim
  SIM_SOURCES src/minesweeper/rules.cpp src/minesweeper/bot.cpp
  SIM_DEPS lam_app lam_input lam_particles lam_render)
