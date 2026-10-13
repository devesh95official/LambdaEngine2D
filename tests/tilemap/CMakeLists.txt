# lam_tilemap tests.
lam_add_test(MODULE tilemap
  SOURCES
    test_smoke.cpp
    test_grid_path.cpp
    test_tilemap.cpp
    test_autotile.cpp
    test_fov.cpp
    test_grid_mover.cpp
    test_grid_maps.cpp
    test_procgen.cpp
    test_plugin.cpp
  DEPS lam_tilemap lam_test_assets)
# The native-format test parses the example in the public header, so the docs can't go stale.
target_compile_definitions(lam_tilemap_tests PRIVATE
  LAM_TILEMAP_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/../../src/tilemap/include/lam/tilemap/tilemap.h")
