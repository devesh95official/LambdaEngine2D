# lam_particles: particle pools, presets and the .particles.json loader. No backend.
lam_add_module(NAME particles
  SOURCES config.cpp pools.cpp loader.cpp plugin.cpp
  DEPS lam_render)
