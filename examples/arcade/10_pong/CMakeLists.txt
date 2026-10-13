# Pong. Rules and bot build into pong_sim (no raylib), drawing lives in the executable.
lam_add_game(NAME pong
  SOURCES src/main.cpp
  SIM_LIBRARY pong_sim
  SIM_SOURCES src/pong/rules.cpp
  SIM_DEPS lam_app lam_ecs lam_input lam_particles lam_render)
