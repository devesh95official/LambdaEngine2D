# lam_core: pure utilities. nlohmann is PRIVATE (data.cpp only).
lam_add_module(NAME core
  SOURCES log.cpp id.cpp hash.cpp text.cpp clock.cpp events.cpp resources.cpp alloc.cpp profiler.cpp
          debug.cpp build_info.cpp fs.cpp data.cpp settings.cpp
  DEPS lam_math)
target_link_libraries(lam_core PRIVATE nlohmann_json::nlohmann_json)
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
                OUTPUT_VARIABLE LAM_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT LAM_GIT_HASH)
  set(LAM_GIT_HASH dev)
endif()
target_compile_definitions(lam_core PRIVATE LAM_BUILD_ID="${LAM_GIT_HASH}")

# Counting operator new for tests and benches. OBJECT so it always links in.
add_library(lam_alloc_hook OBJECT alloc_hook.cpp)
target_link_libraries(lam_alloc_hook PUBLIC lam_core)
lam_target_defaults(lam_alloc_hook)
