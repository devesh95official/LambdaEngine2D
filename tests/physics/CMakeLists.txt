# lam_physics tests and bench. The tests include the private detail/ headers to test internals.
lam_add_test(MODULE physics
  SOURCES test_smoke.cpp test_tile_sweep.cpp test_bodies.cpp test_motors.cpp test_steering.cpp test_queries.cpp test_movement.cpp test_broadphase.cpp test_platformer_sweep.cpp
          test_game_usage.cpp test_regressions.cpp test_tile_world.cpp test_topdown_crowds.cpp
  DEPS lam_physics)
target_include_directories(lam_physics_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/physics)
# lam_physics_bench has its own main (--bodies, --ticks), so it's a plain executable. It still
# links the allocation hook to report allocations.
add_executable(lam_physics_bench bench_physics.cpp)
target_link_libraries(lam_physics_bench PRIVATE lam_physics)
if(NOT LAM_SANITIZE)
  target_link_libraries(lam_physics_bench PRIVATE lam_alloc_hook)
endif()
lam_target_defaults(lam_physics_bench)
add_test(NAME lam_physics_bench COMMAND lam_physics_bench --bodies 10000 --ticks 600)
set_tests_properties(lam_physics_bench PROPERTIES LABELS "physics;bench")
