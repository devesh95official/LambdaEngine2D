# Frogger. Rules and bot build into frogger_sim (no raylib), drawing lives in the executable.
lam_add_game(NAME frogger
  SOURCES src/main.cpp
  SIM_LIBRARY frogger_sim
  SIM_SOURCES src/frogger/rules.cpp src/frogger/bot.cpp
  SIM_DEPS lam_app lam_ecs lam_input lam_particles lam_render)
