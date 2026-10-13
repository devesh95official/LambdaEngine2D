# lam_roguelike_tests, ctest label "roguelike". lam_engine provides DefaultPlugins; GAME_ASSETS_DIR
# lets the test find prefabs.json from any working directory.
lam_add_test(MODULE roguelike SOURCES test_rules.cpp DEPS roguelike_sim lam_engine)
target_compile_definitions(lam_roguelike_tests PRIVATE GAME_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
