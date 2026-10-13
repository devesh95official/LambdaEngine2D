# lam_top_down_shooter_tests, ctest label top_down_shooter. The test finds ../assets through
# LAM_TEST_SOURCE_DIR (set by lam_add_test).
lam_add_test(MODULE top_down_shooter SOURCES test_rules.cpp DEPS top_down_shooter_sim lam_engine)
