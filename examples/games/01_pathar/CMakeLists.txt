# Pathar: a Sokoban + water grid puzzle. Rules live in pathar_sim, which links only pure engine
# targets, so tests and the bot run without a window; presentation (tiles, sprites, HUD, audio,
# menus) lives in the executable.
lam_add_game(NAME pathar
  SOURCES
    src/main.cpp
    src/game/game.cpp
    src/game/look.cpp
    src/game/scenes.cpp
  ASSETS assets
  SIM_LIBRARY pathar_sim
  SIM_SOURCES
    src/sim/rules.cpp
    src/sim/catalogue.cpp
    src/sim/board.cpp
    src/sim/sim_plugin.cpp
    src/sim/bot.cpp
  SIM_DEPS lam_ecs lam_tilemap lam_input lam_audio lam_particles)

# Tests (doctest via lam_test_support): pure rules (incl. the solver's recorded answers), the ECS
# board on a bare World, and headless-App runs of the bot, the controls and SaveSlots.
if(TARGET lam_test_support)
  lam_add_test(MODULE pathar NAME pathar_tests
    SOURCES tests/rules_test.cpp tests/world_test.cpp tests/bot_test.cpp
    DEPS pathar_sim lam_scene lam_engine)
  target_compile_definitions(pathar_tests PRIVATE "PATHAR_ASSETS=\"${CMAKE_CURRENT_SOURCE_DIR}/assets\"")
  target_include_directories(pathar_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
endif()
