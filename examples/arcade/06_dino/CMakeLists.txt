# Dino runner. Rules and bot build into dino_sim (no raylib), drawing lives in the executable.
lam_add_game(NAME dino
  SOURCES src/main.cpp
  SIM_LIBRARY dino_sim
  SIM_SOURCES src/dino/rules.cpp
  SIM_DEPS lam_app lam_ecs lam_input lam_particles lam_render)
