# physics_demo: a platformer hero on a hand-built tile world, falling bodies, a lift
# and a trigger (see main.cpp). Windowed by default; --frames N --hidden and --headless for scripted runs.
lam_add_example(NAME physics_demo SOURCES main.cpp DEPS lam_engine)
