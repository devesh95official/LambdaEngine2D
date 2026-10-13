# input_demo: the lam_input API in one file. Windowed by default (lam_platform +
# lam_input_rl); --headless runs a bot, records and replays in-process. No assets.
lam_add_example(NAME input_demo SOURCES main.cpp DEPS lam_input_rl lam_platform)
