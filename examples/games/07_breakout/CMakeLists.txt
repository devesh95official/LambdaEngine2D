# Breakout: shapes only, written from docs/RECIPES.md. Rules and bot are in breakout_sim (pure
# targets) so they run in tests and --headless.
lam_add_game(NAME breakout
  SOURCES main.cpp
  SIM_LIBRARY breakout_sim
  SIM_SOURCES src/breakout/rules.cpp src/breakout/bot.cpp
  SIM_DEPS lam_app lam_physics lam_input)
if(LAM_BUILD_TESTS AND TARGET lam_test_support)
  add_subdirectory(tests)
endif()
