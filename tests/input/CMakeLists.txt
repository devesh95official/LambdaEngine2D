# lam_input tests. Devices are faked with InputSystem::feed_raw; StepApp runs frames with an
# exact tick count (0..4).
lam_add_test(MODULE input
  SOURCES test_smoke.cpp test_latch.cpp test_repeat.cpp test_axes.cpp test_actions.cpp
          test_pointer_touch.cpp test_intents_driver.cpp test_replay.cpp test_alloc.cpp test_regressions.cpp
  DEPS lam_input)
