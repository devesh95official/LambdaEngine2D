# render_demo: lam_render in one screen (sprites from one sheet, y-sort, shadows,
# camera follow + shake, text, F3 overlay). assets/sprites.png is written by make_assets.py.
#   render_demo [--sprites N] [--frames N] [--hidden] [--headless] [--overlay]
lam_add_example(NAME render_demo SOURCES main.cpp DEPS lam_render_rl lam_platform ASSETS assets)
