# lam_tilemap: tile maps, autotiling, pathfinding, FOV, grid movement and procgen. No raylib.
lam_add_module(NAME tilemap
  SOURCES
    autotile.cpp
    fov.cpp
    grid_maps.cpp
    grid_mover.cpp
    grid_path.cpp
    native_map.cpp
    path_follow.cpp
    procgen.cpp
    tiled_map.cpp
    tilemap.cpp
    tilemap_plugin.cpp
    tileset.cpp
  DEPS lam_render lam_physics)
