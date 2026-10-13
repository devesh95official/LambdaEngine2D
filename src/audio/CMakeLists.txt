# lam_audio has no raylib dependency. lam_audio_rl adds the raylib device and plugin.
lam_add_module(NAME audio
  SOURCES audio.cpp audio_streams.cpp audio_plugin.cpp loaders.cpp null_device.cpp sound_math.cpp
  DEPS lam_assets)
target_include_directories(lam_audio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

lam_add_backend(NAME audio SOURCES backend/raylib_audio.cpp DEPS lam_app)
