# lam_app tests and the shared harness's own tests.
lam_add_test(MODULE app NAME lam_app_tests
  SOURCES test_app_stages.cpp test_app_worlds.cpp test_app_events.cpp test_app_plugins.cpp
          test_app_options.cpp test_app_run.cpp test_app_additions.cpp test_support.cpp
  DEPS lam_app)
