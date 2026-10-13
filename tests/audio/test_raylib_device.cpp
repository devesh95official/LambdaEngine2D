// lam_audio_rl against the real raylib audio device (label gpu: needs a hidden window). Without
// audio hardware the device is silent and the test checks that path instead.
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/audio/audio_plugin.h>
#include <lam/audio/device.h>
#include <lam/core/app.h>
#include <lam/core/raylib_platform.h>
#include <lam/test/support.h>

#include "audio_test_util.h"

using lam::Audio;
using lam::PlayParams;
using lam::VoiceHandle;

namespace {

// Hidden-window App with just the audio part of DefaultPlugins.
struct RlFixture {
    lam::test::TempDir dir;
    std::unique_ptr<lam::App> app;
    RlFixture() {
        const char *argv[] = {"lam_audio_rl_tests", "--hidden"};
        app = std::make_unique<lam::App>(
            lam::AppConfig{.title = "lam_audio_rl_tests", .window_size = {320, 180}, .settings_file = ""}, 2, argv);
        app->add_plugin<lam::PlatformPlugin>();
        lam::AssetsConfig c;
        c.root = dir.path();
        app->add_plugin<lam::AssetsPlugin>(c);
        app->add_plugin<lam::AudioPlugin>();
        app->add_plugin<lam::RaylibAudioPlugin>();
    }
    Audio &audio() { return app->res<Audio>(); }
    lam::AssetServer &assets() { return app->res<lam::AssetServer>(); }
    template <class T> lam::AssetId<T> load(std::string_view name, float seconds) {
        audio_test::write_bytes(dir.file(name), audio_test::make_wav(seconds, 11025, 1, 16));
        auto r = assets().load_now<T>(name);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
        return *r;
    }
    // Runs frames (real time) until done() or max_seconds of wall time; true if done.
    template <class F> bool frames_until(F done, double max_seconds = 3.0) {
        const auto start = std::chrono::steady_clock::now();
        while (!done()) {
            if (std::chrono::steady_clock::now() - start > std::chrono::duration<double>(max_seconds)) return false;
            app->run_frames(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
};

} // namespace

TEST_CASE("raylib device: clips, aliases, loops, reaping, streams, stingers, suspend, shutdown") {
    RlFixture f;
    Audio &audio = f.audio();
    REQUIRE(dynamic_cast<lam::NullAudioDevice *>(&audio.device()) == nullptr); // RaylibAudioDevice installed
    const auto clip = f.load<lam::SoundClip>("blip.wav", 0.15f);
    const lam::SoundClip *c = f.assets().get(clip);
    REQUIRE(c != nullptr);
    CHECK(c->encoded.empty()); // freed after the upload

    if (!audio.device().ready()) {
        MESSAGE("no audio hardware: RaylibAudioDevice is silent; checking the silent path");
        CHECK(c->device_id == 0);
        CHECK_FALSE(audio.play(clip));
        CHECK(audio.plays() == 1);
        return;
    }
    REQUIRE(c->device_id != 0);
    audio.set_clip_min_interval(0);

    // One-shots: playing until the clip ends, then reaped by update().
    const VoiceHandle one = audio.play(clip, {.volume = 0.2f});
    REQUIRE(one);
    CHECK(audio.playing(one));
    CHECK(audio.device().slot_playing(static_cast<int>(one.index - 1)));
    CHECK(f.frames_until([&] { return !audio.playing(one); }));
    CHECK(audio.active_voices() == 0);

    // One sound on several slots at once (aliases), all audible together.
    VoiceHandle many[6];
    for (VoiceHandle &v : many) v = audio.play(clip, {.volume = 0.05f});
    for (const VoiceHandle &v : many) CHECK(audio.device().slot_playing(static_cast<int>(v.index - 1)));
    CHECK(audio.active_voices() == 6);
    for (const VoiceHandle &v : many) audio.stop(v);
    CHECK(audio.active_voices() == 0);

    // A looping voice outlives its clip (restarted) until stopped.
    const VoiceHandle loop = audio.play(clip, {.volume = 0.05f, .loop = true});
    const double t0 = audio.real_time();
    CHECK(f.frames_until([&] { return audio.real_time() - t0 > 0.5; }));
    CHECK(audio.playing(loop));
    audio.stop(loop, 0.1f);
    CHECK(f.frames_until([&] { return !audio.playing(loop); }));

    // Steady-state playback through the raylib device allocates nothing (aliases are reused).
    for (int i = 0; i < 3; ++i) {
        for (VoiceHandle &v : many) v = audio.play(clip, {.volume = 0.01f});
        audio.update(1.0f / 60);
        for (const VoiceHandle &v : many) audio.stop(v);
    }
    CHECK_NO_ALLOC({
        for (int i = 0; i < 120; ++i) {
            for (VoiceHandle &v : many) v = audio.play(clip, {.volume = 0.01f, .pitch = 1.1f, .pan = 0.3f});
            audio.update(1.0f / 60);
            for (const VoiceHandle &v : many) audio.stop(v);
        }
    });

    // Music streams from memory; a stinger ducks it and ends by itself (device end of stream).
    const auto music = f.load<lam::MusicTrack>("music.wav", 1.0f);
    const auto sting = f.load<lam::MusicTrack>("sting.wav", 0.2f);
    audio.set_bus_volume(audio.bus("music"), 0.05f);
    audio.play_music(music, 0.0f);
    f.app->run_frames(1);
    CHECK(audio.track_gain(music) == doctest::Approx(1.0f));
    audio.play_stinger(sting, 0.25f);
    CHECK(f.frames_until([&] { return audio.music_duck() < 0.3f; }));
    CHECK(f.frames_until([&] { return audio.track_gain(sting) == 0.0f; }));   // ended and closed
    CHECK(f.frames_until([&] { return audio.music_duck() > 0.99f; }));
    CHECK(audio.track_gain(music) == doctest::Approx(1.0f));                  // looping: still on

    // Suspend pauses the device: voices are kept (not reaped) and resume where they were.
    const auto long_clip = f.load<lam::SoundClip>("long.wav", 1.5f);
    const VoiceHandle held = audio.play(long_clip, {.volume = 0.05f});
    audio.set_suspended(true);
    CHECK_FALSE(audio.device().slot_playing(static_cast<int>(held.index - 1)));
    const double t1 = audio.real_time();
    CHECK(f.frames_until([&] { return audio.real_time() - t1 > 0.3; }));
    CHECK(audio.playing(held));
    audio.set_suspended(false);
    CHECK(audio.device().slot_playing(static_cast<int>(held.index - 1)));

    // Shut down with voices and streams live. They must be unloaded before the platform closes the
    // device; ASan catches a use after free here.
    f.app.reset();
}
