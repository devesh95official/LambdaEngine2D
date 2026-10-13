# lam_assets tests, covering both the synchronous and the asynchronous server.
lam_add_test(MODULE assets
  SOURCES test_smoke.cpp test_sync.cpp test_scopes.cpp test_async.cpp test_hot_reload.cpp
          test_root.cpp test_edges.cpp test_budget.cpp test_regressions.cpp
  DEPS lam_assets)
# test_regressions.cpp checks a private helper (detail/asset_internal.h).
target_include_directories(lam_assets_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/assets)
