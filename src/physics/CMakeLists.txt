# lam_physics (no raylib).
lam_add_module(NAME physics
  SOURCES shapes.cpp tile_world.cpp tile_rays.cpp broadphase.cpp solver.cpp motors.cpp projectiles.cpp
          queries.cpp steering.cpp physics_plugin.cpp
  DEPS lam_app)
target_include_directories(lam_physics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
