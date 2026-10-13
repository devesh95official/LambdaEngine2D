# lam_ecs tests. lam_ecs_bench measures the ECS budget; label bench.
lam_add_test(MODULE ecs
  SOURCES test_entities.cpp test_views.cpp test_commands.cpp test_hierarchy.cpp test_snapshot.cpp
          test_reflection.cpp test_prefab.cpp test_turns.cpp test_alloc.cpp
  DEPS lam_ecs)
if(LAM_BUILD_BENCH)
  lam_add_test(MODULE ecs NAME lam_ecs_bench SOURCES bench_ecs.cpp DEPS lam_ecs LABELS bench)
endif()
