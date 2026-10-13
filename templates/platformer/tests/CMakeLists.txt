# lam_platformer_tests, ctest label "platformer". Links the sim library only (no raylib).
lam_add_test(MODULE platformer SOURCES test_rules.cpp DEPS platformer_sim)
