# Space Invaders. Rules and bot build into invaders_sim (no raylib), drawing lives in the executable.
lam_add_game(NAME invaders
  SOURCES src/main.cpp
  SIM_LIBRARY invaders_sim
  SIM_SOURCES src/invaders/rules.cpp src/invaders/bot.cpp
  SIM_DEPS lam_app lam_ecs lam_input lam_particles lam_render)
