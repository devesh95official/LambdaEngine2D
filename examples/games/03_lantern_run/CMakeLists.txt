# Lantern Run: top-down chase at night. Rules live in lantern_run_sim
# (pure lam_* targets only, testable headless); presentation (camera, lights, particles, HUD)
# lives in the executable. lam_render is a pure target (data types, no raylib): choose_clips writes
# Sprite::kFlipX, the one presentation flag the 4-direction clips need from a tick system.
lam_add_game(NAME lantern_run
  SOURCES
    src/main.cpp
    src/view/view.cpp
    src/view/hud.cpp
  ASSETS assets
  SIM_LIBRARY lantern_run_sim
  SIM_SOURCES
    src/sim/anim.cpp
    src/sim/bot.cpp
    src/sim/lanterns.cpp
    src/sim/level.cpp
    src/sim/player.cpp
    src/sim/plugin.cpp
    src/sim/run.cpp
    src/sim/util.cpp
    src/sim/wolves.cpp
  SIM_DEPS lam_ecs lam_assets lam_physics lam_tilemap lam_render lam_animation lam_input)

if(LAM_BUILD_TESTS AND TARGET lam_test_support)
  add_subdirectory(tests)
endif()
