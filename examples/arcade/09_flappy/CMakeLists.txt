# Flappy. Rules and bot build into flappy_sim (no raylib), drawing lives in the executable.
lam_add_game(NAME flappy
  SOURCES src/main.cpp
  SIM_LIBRARY flappy_sim
  SIM_SOURCES src/flappy/rules.cpp
  SIM_DEPS lam_app lam_ecs lam_input lam_particles lam_render)
