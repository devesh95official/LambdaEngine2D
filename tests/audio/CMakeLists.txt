# lam_audio tests. Headless, against NullAudioDevice or small test devices, with generated WAVs.
lam_add_test(MODULE audio
  SOURCES test_smoke.cpp test_spatial.cpp test_buses.cpp test_voices.cpp test_banks.cpp
          test_music.cpp test_world.cpp test_regressions.cpp
  DEPS lam_audio)
# RaylibAudioDevice on real hardware. Opens a hidden window, so it's labelled gpu (ctest -L gpu).
if(TARGET lam_platform AND TARGET lam_audio_rl)
  lam_add_test(MODULE audio NAME lam_audio_rl_tests SOURCES test_raylib_device.cpp
    DEPS lam_audio_rl lam_platform LABELS gpu)
endif()
