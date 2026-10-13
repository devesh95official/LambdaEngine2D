# Platformer rule tests (lam_platformer_tests, label "platformer"): pure checks (bot decisions,
# scoring) and full-level runs.
lam_add_test(MODULE platformer SOURCES test_platformer.cpp DEPS platformer_sim lam_engine lam_scene)
target_compile_definitions(lam_platformer_tests PRIVATE
                           PLATFORMER_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../assets")
