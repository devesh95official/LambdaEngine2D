# lam_render tests.
#   lam_render_tests      pure, headless (default ctest run)
#   lam_render_gpu_tests  hidden 640x360 window: draw calls, golden image, no allocation (label gpu)
#   lam_render_cpu_bench  collect + plan of the bench scene, no window (label bench; LAM_BUILD_BENCH)
#   lam_render_bench      windowed throughput bench: --sprites N --frames 600 --no-vsync (label bench;
#                         LAM_BUILD_BENCH)
lam_add_test(MODULE render
  SOURCES test_smoke.cpp test_batching.cpp test_camera.cpp test_fade.cpp test_shake.cpp
          test_virtual.cpp test_systems.cpp test_text.cpp test_materials.cpp test_game_usage.cpp
  DEPS lam_render lam_test_assets)
target_include_directories(lam_render_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/render)

lam_add_test(MODULE render NAME lam_render_gpu_tests SOURCES test_gpu.cpp
  DEPS lam_render_rl lam_platform lam_test_assets LABELS gpu)
target_compile_definitions(lam_render_gpu_tests PRIVATE
  LAM_RENDER_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
# A TrueType font for the .ttf baking test (raylib's example resources; skipped when absent).
if(DEFINED raylib_SOURCE_DIR)
  target_compile_definitions(lam_render_gpu_tests PRIVATE
    LAM_RENDER_TEST_TTF="${raylib_SOURCE_DIR}/examples/text/resources/anonymous_pro_bold.ttf")
endif()

if(LAM_BUILD_BENCH)
  lam_add_test(MODULE render NAME lam_render_cpu_bench SOURCES bench_render_cpu.cpp
    DEPS lam_render LABELS bench)
  # Its own main (it takes --sprites N --frames N --no-vsync [--hidden]), so not lam_add_test.
  add_executable(lam_render_bench bench_render.cpp)
  target_link_libraries(lam_render_bench PRIVATE lam_render_rl lam_platform)
  if(NOT LAM_SANITIZE)
    target_link_libraries(lam_render_bench PRIVATE lam_alloc_hook)
  endif()
  lam_target_defaults(lam_render_bench)
  add_test(NAME lam_render_bench COMMAND lam_render_bench --sprites 10000 --frames 600 --no-vsync
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(lam_render_bench PROPERTIES LABELS "render;bench")
  # --help prints the usage and exits 0 without opening a window.
  add_test(NAME lam_render_bench_help COMMAND lam_render_bench --help)
  set_tests_properties(lam_render_bench_help PROPERTIES LABELS "render"
                       PASS_REGULAR_EXPRESSION "^usage: lam_render_bench")
endif()
