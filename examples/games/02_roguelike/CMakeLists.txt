# Roguelike: a turn-based dungeon crawl. Rules are in roguelike_sim (no raylib), presentation in the
# executable. Run: roguelike [--bot] [--headless --frames N --seed S --hash-every N]
lam_add_game(NAME roguelike
  SOURCES src/game/main.cpp src/game/scenes.cpp src/game/view.cpp
  ASSETS assets
  SIM_LIBRARY roguelike_sim
  SIM_SOURCES src/sim/data.cpp src/sim/controls.cpp src/sim/run.cpp src/sim/dungeon.cpp src/sim/turns.cpp
              src/sim/combat.cpp src/sim/items.cpp src/sim/save.cpp src/sim/bot.cpp
  SIM_DEPS lam_ecs lam_assets lam_tilemap lam_input lam_scene lam_ui)

if(TARGET lam_test_support)
  lam_add_test(MODULE roguelike SOURCES tests/test_roguelike.cpp DEPS roguelike_sim lam_engine)
endif()
