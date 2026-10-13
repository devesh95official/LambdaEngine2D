# lam_core unit tests, plus the include-graph and header-reference checks.
# lam_app_tests live in tests/core/app/.
lam_add_test(MODULE core
  SOURCES test_result.cpp test_id.cpp test_hash.cpp test_fixed_string.cpp test_text.cpp
          test_clock.cpp test_events.cpp test_resources.cpp test_data.cpp test_fs.cpp
          test_settings.cpp test_alloc.cpp test_fsm.cpp test_catalog.cpp test_profiler.cpp
          test_debug.cpp test_build_info.cpp test_include_graph.cpp test_header_refs.cpp
  DEPS lam_core)
# The structural tests scan the source tree. test_alloc needs to know if the hook is linked.
target_compile_definitions(lam_core_tests PRIVATE
  LAM_REPO_ROOT="${PROJECT_SOURCE_DIR}"
  LAM_TEST_SANITIZE=$<BOOL:${LAM_SANITIZE}>)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app/CMakeLists.txt)
  add_subdirectory(app)
endif()
