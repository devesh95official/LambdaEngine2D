# audio_demo: a sound bank with variants, a looping emitter circling the listener, PlaySound from a
# tick system, crossfaded music, a stinger, ambience and bus volumes. WAVs come from make_sounds.py.
# Runs windowed, --hidden --frames N, or --headless (NullAudioDevice).
lam_add_example(NAME audio_demo SOURCES main.cpp DEPS lam_engine ASSETS assets)
