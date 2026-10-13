# Rules tests: headless, against lantern_run_sim (+ lam_engine for the full-engine bot runs).
# Every case runs; the full-engine ones use DefaultPlugins{.headless = true}.
lam_add_test(MODULE lantern_run
  SOURCES test_rules.cpp test_bot.cpp
  DEPS lantern_run_sim lam_engine)
target_compile_definitions(lam_lantern_run_tests PRIVATE LANTERN_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/../assets")
