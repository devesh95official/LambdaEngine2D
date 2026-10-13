# Shooter rule tests (target lam_shooter_tests). They build the same headless App as
# `shooter --headless --bot` (DefaultPlugins{.headless = true}) with the asset root pointed at
# ../assets through LAM_TEST_SOURCE_DIR (set by lam_add_test).
lam_add_test(MODULE shooter SOURCES test_shooter.cpp DEPS shooter_sim lam_engine lam_scene)
