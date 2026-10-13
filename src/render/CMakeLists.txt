# lam_render has no raylib dependency. lam_render_rl is the raylib backend (uploads, the sprite
# batcher, present, screenshots).
lam_add_module(NAME render
  SOURCES texture.cpp render_queue.cpp camera_view.cpp virtual_screen.cpp atlas.cpp text.cpp
          batching.cpp camera_math.cpp fade.cpp lighting.cpp passes.cpp shader.cpp fonts.cpp
          render_systems.cpp collect.cpp render_plugin.cpp shapes.cpp
  DEPS lam_assets)
target_include_directories(lam_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

lam_add_backend(NAME render
  SOURCES backend/raylib_render_plugin.cpp backend/gpu_batcher.cpp backend/rl_loaders.cpp)
target_include_directories(lam_render_rl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
