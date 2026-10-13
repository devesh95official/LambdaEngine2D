# scene_demo: scene stack (title -> play -> pause overlay -> back) with a save and a
# load of the play world. Scripted; `scene_demo --headless` runs to completion and exits 0.
lam_add_example(NAME scene_demo SOURCES main.cpp DEPS lam_engine)
