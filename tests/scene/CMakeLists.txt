# lam_scene tests: the scene stack, save envelope and slots, and world serialisation.
lam_add_test(MODULE scene
  SOURCES test_smoke.cpp test_scene_stack.cpp test_save.cpp test_world_save.cpp test_scene_save.cpp test_regressions.cpp
  DEPS lam_scene lam_test_assets)
