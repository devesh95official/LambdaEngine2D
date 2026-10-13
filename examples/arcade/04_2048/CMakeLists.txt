# 2048. Rules and bot build into g2048_sim (no raylib), drawing lives in the executable.
lam_add_game(NAME g2048
  SOURCES src/main.cpp
  SIM_LIBRARY g2048_sim
  SIM_SOURCES src/g2048/rules.cpp
  SIM_DEPS lam_app lam_input lam_particles lam_render)
