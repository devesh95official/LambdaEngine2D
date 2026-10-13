# Missile Command. Rules and bot build into missile_sim (no raylib), drawing lives in the executable.
lam_add_game(NAME missile
  SOURCES src/main.cpp
  SIM_LIBRARY missile_sim
  SIM_SOURCES src/game/rules.cpp src/game/bot.cpp
  SIM_DEPS lam_app lam_ecs lam_input lam_particles lam_audio lam_render)
