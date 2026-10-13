# lam_animation tests. White-box tests include src/animation/detail/*.h.
# lam_animation_bench (label bench, LAM_BUILD_BENCH) times 10k animated heroes per tick.
lam_add_test(MODULE animation
  SOURCES test_smoke.cpp test_sprite_sheet.cpp test_animator.cpp test_anim_graph.cpp test_tween.cpp
          test_plugin.cpp
  DEPS lam_animation lam_test_assets)
target_include_directories(lam_animation_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/animation)
if(LAM_BUILD_BENCH)
  lam_add_test(MODULE animation NAME lam_animation_bench SOURCES bench_animation.cpp
               DEPS lam_animation lam_test_assets LABELS bench)
endif()
