# Top-down arena shooter. Rules are in shooter_sim (pure lam_* targets, no raylib); HUD, camera,
# menus, fx assets and --bot wiring are in the shooter executable.
lam_add_game(NAME shooter
  SOURCES
    src/main.cpp
    src/present/presentation.cpp
    src/present/scenes.cpp
  ASSETS assets
  SIM_LIBRARY shooter_sim
  SIM_SOURCES
    src/shooter/arena.cpp
    src/shooter/bot.cpp
    src/shooter/crowd.cpp
    src/shooter/rules.cpp
    src/shooter/sim_plugin.cpp
    src/shooter/waves.cpp
  SIM_DEPS lam_app lam_ecs lam_physics lam_tilemap lam_input lam_particles lam_audio)

if(LAM_BUILD_TESTS AND TARGET lam_test_support)
  add_subdirectory(tests)
endif()
