# assets_demo: a custom asset type and loader with a dependency, asynchronous
# loading, AssetEvent reactions, scopes and hot reload. Windowed through
# lam_platform; draw_rl.cpp is its only raylib file. --headless runs it in the console.
lam_add_example(NAME assets_demo SOURCES main.cpp draw_rl.cpp
  DEPS lam_assets lam_platform lam_rl_common ASSETS assets)
