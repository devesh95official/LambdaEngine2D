# lam_input has no raylib dependency; lam_input_rl holds raw_input_poll and RaylibInputPlugin.
lam_add_module(NAME input
  SOURCES keys.cpp action_map.cpp bindings.cpp input_system.cpp replay.cpp input_plugin.cpp
  DEPS lam_render)
lam_add_backend(NAME input SOURCES backend/raylib_input.cpp)
