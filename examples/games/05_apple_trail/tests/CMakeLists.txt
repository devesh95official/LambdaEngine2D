# Rules tests for Apple Trail: the sim library (+ lam_scene for the save round trip); no window, no raylib.
lam_add_test(MODULE apple_trail SOURCES test_rules.cpp DEPS apple_trail_sim lam_scene)
