# lam_test_support: doctest main and <lam/test/support.h>, linked by every lam_add_test target.
# lam_test_assets: asset fixtures (<lam/test/assets.h>), built once lam_assets and lam_render exist.
add_library(lam_test_support STATIC support.cpp)
target_include_directories(lam_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lam_test_support PUBLIC lam_app doctest::doctest)
target_compile_definitions(lam_test_support PUBLIC DOCTEST_CONFIG_VOID_CAST_EXPRESSIONS)
target_compile_features(lam_test_support PUBLIC cxx_std_20)
lam_target_defaults(lam_test_support)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets.cpp AND TARGET lam_assets AND TARGET lam_render)
  add_library(lam_test_assets STATIC assets.cpp)
  target_link_libraries(lam_test_assets PUBLIC lam_test_support lam_assets lam_render)
  lam_target_defaults(lam_test_assets)
endif()
