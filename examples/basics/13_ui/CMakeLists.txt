# ui_demo: a main menu (buttons, slider, toggle, choice) and a scrolling level
# list in a UiTree, reacting with toasts and a message log; attract mode until the first input.
#   ui_demo [--frames N] [--hidden] [--headless]
# Generated assets: python3 make_assets.py rewrites assets/ui/panel.png.
lam_add_example(NAME ui_demo SOURCES main.cpp DEPS lam_engine ASSETS assets)
