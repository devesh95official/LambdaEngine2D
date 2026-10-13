# Platformer: the docs/RECIPES.md skeleton grown into a level. Rules, level spawning and the bot are
# in platformer_sim (pure targets); scenes, camera, HUD and fx are in the executable.
lam_add_game(NAME platformer
             SOURCES main.cpp
             ASSETS assets
             SIM_LIBRARY platformer_sim
             SIM_SOURCES src/platformer/rules.cpp src/platformer/level.cpp src/platformer/bot.cpp
             SIM_DEPS lam_app lam_assets lam_physics lam_tilemap lam_animation lam_input lam_particles
                      lam_audio)
if(LAM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
