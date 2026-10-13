# lam_engine: the DefaultPlugins bundle. It links every pure and backend
# target, so a game links lam_engine alone. The root adds this folder after every module.
set(lam_engine_deps "")
foreach(t IN ITEMS lam_app lam_platform lam_assets lam_render lam_render_rl lam_input lam_input_rl
               lam_audio lam_audio_rl lam_physics lam_scene lam_tilemap lam_animation
               lam_particles lam_ui)
  if(TARGET ${t})
    list(APPEND lam_engine_deps ${t})
  endif()
endforeach()
lam_add_module(NAME engine TARGET lam_engine SOURCES default_plugins.cpp DEPS ${lam_engine_deps})
