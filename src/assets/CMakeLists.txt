# lam_assets: the asset server (loaders, scopes, GC, decode worker, hot reload). No raylib.
lam_add_module(NAME assets
  SOURCES asset_server.cpp asset_registry.cpp asset_pipeline.cpp asset_decode.cpp asset_worker.cpp
          asset_hot_reload.cpp asset_paths.cpp asset_root.cpp
  DEPS lam_app)
find_package(Threads REQUIRED)
target_link_libraries(lam_assets PUBLIC Threads::Threads)
