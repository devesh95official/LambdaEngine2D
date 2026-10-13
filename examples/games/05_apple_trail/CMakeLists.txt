# Apple Trail: a snake-style game. See README.md.
# The rules (src/) build into apple_trail_sim, which links only pure engine targets, so tests and
# tools can run the game with no window. The executable (game/) adds scenes and presentation.
lam_add_game(NAME apple_trail
             SOURCES game/main.cpp game/scenes.cpp game/present.cpp
             ASSETS assets
             SIM_LIBRARY apple_trail_sim
             SIM_SOURCES src/trail_rules.cpp src/trail_map.cpp src/trail_bot.cpp
             SIM_DEPS lam_tilemap lam_particles lam_input)
if(LAM_BUILD_TESTS AND TARGET lam_test_support)
  add_subdirectory(tests)
endif()
