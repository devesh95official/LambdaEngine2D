# lam_animation: sprite sheets, animators, animation graphs and tweens. No backend.
lam_add_module(NAME animation
  SOURCES sprite_sheet.cpp anim_graph.cpp animator.cpp tween.cpp animation_plugin.cpp
  DEPS lam_render lam_physics)
