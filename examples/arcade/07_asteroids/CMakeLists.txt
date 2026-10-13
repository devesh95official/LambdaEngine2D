# Asteroids. Rules and bot build into asteroids_sim (no raylib), drawing lives in the executable.
lam_add_game(NAME asteroids
  SOURCES src/main.cpp
  SIM_LIBRARY asteroids_sim
  SIM_SOURCES src/asteroids/rules.cpp src/asteroids/bot.cpp
  SIM_DEPS lam_app lam_ecs lam_input lam_particles lam_render)
