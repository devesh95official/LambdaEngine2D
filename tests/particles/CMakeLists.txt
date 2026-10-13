# lam_particles tests.
lam_add_test(MODULE particles
  SOURCES test_smoke.cpp test_config.cpp test_pools.cpp test_draw.cpp test_loader.cpp test_plugin.cpp
  DEPS lam_particles lam_test_assets)
if(LAM_BUILD_BENCH)
  lam_add_test(MODULE particles NAME lam_particles_bench SOURCES bench_particles.cpp DEPS lam_particles LABELS bench)
endif()
