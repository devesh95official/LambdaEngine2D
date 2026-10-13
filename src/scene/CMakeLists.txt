# lam_scene: scene stack, saves and world serialisation. No raylib; only scene_fade.cpp uses
# lam_render.
lam_add_module(NAME scene
  SOURCES scene.cpp scene_fade.cpp save.cpp world_save.cpp
  DEPS lam_render)
