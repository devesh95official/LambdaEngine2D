// Streams: music, stingers, ducking and ambience.
#include <cstdint>
#include <string>

#include <lam/audio/audio.h>
#include <lam/test/support.h>

#include "audio_test_util.h"

using namespace audio_test;
using lam::Audio;

TEST_CASE("music tracks keep their bytes for streaming from memory and an absolute path") {
    Fixture f;
    const auto t = f.track("music/m.wav", 1.0f);
    const lam::MusicTrack *m = f.assets().get(t);
    REQUIRE(m != nullptr);
    CHECK(m->seconds == doctest::Approx(1.0f));
    CHECK_FALSE(m->encoded.empty());
    CHECK(m->extension == ".wav");
    CHECK(m->path == f.assets().resolve_path("music/m.wav"));
    CHECK(m->path.front() == '/');
}

TEST_CASE("music: opens a looping stream from memory and fades in over the crossfade") {
    Fixture f;
    Audio &a = f.audio();
    const auto ta = f.track("a.wav");
    a.play_music(ta, 1.0f);
    CHECK(a.music() == ta);
    const std::uint32_t id = last_stream(f.device());
    REQUIRE(id != 0);
    CHECK(last_of(f.device(), Kind::StreamPlay).id == id);
    CHECK(last_of(f.device(), Kind::StreamPlay).slot == 1); // looping
    CHECK(a.track_gain(ta) == 0.0f);
    CHECK(stream_volume(f.device(), id) == 0.0f); // the volume is set before it starts
    a.update(0.5f);
    CHECK(a.track_gain(ta) == doctest::Approx(0.5f));
    CHECK(stream_volume(f.device(), id) == doctest::Approx(0.5f));
    a.update(0.5f);
    CHECK(a.track_gain(ta) == doctest::Approx(1.0f));
    a.update(1.0f);
    CHECK(a.track_gain(ta) == doctest::Approx(1.0f));
}

TEST_CASE("music: a crossfade moves both tracks linearly and closes the old one") {
    Fixture f;
    Audio &a = f.audio();
    const auto ta = f.track("a.wav");
    const auto tb = f.track("b.wav");
    a.play_music(ta, 0.0f);
    const std::uint32_t id_a = last_stream(f.device());
    a.update(0.0f);
    CHECK(a.track_gain(ta) == 1.0f);
    a.play_music(tb, 1.0f);
    const std::uint32_t id_b = last_stream(f.device());
    CHECK(id_b != id_a);
    a.update(0.25f);
    CHECK(a.track_gain(ta) == doctest::Approx(0.75f));
    CHECK(a.track_gain(tb) == doctest::Approx(0.25f));
    a.update(0.75f);
    CHECK(a.track_gain(ta) == 0.0f);
    CHECK(a.track_gain(tb) == doctest::Approx(1.0f));
    CHECK(last_of(f.device(), Kind::CloseStream).id == id_a);
    CHECK(a.music() == tb);

    const std::size_t opened = f.device().count(Kind::OpenStream);
    a.play_music(tb, 1.0f); // the same track again: nothing
    CHECK(f.device().count(Kind::OpenStream) == opened);

    a.play_music({}, 0.5f); // none: fade out
    CHECK_FALSE(a.music());
    a.update(0.25f);
    CHECK(a.track_gain(tb) == doctest::Approx(0.5f));
    a.update(0.25f);
    CHECK(a.track_gain(tb) == 0.0f);
    CHECK(last_of(f.device(), Kind::CloseStream).id == id_b);
}

TEST_CASE("music: going back to a fading track fades it back in without reopening it") {
    Fixture f;
    Audio &a = f.audio();
    const auto ta = f.track("a.wav");
    const auto tb = f.track("b.wav");
    a.play_music(ta, 0.0f);
    a.update(0.0f);
    a.play_music(tb, 1.0f);
    a.update(0.5f);
    const std::size_t opened = f.device().count(Kind::OpenStream);
    a.play_music(ta, 1.0f);
    CHECK(f.device().count(Kind::OpenStream) == opened);
    a.update(0.5f);
    CHECK(a.track_gain(ta) == doctest::Approx(0.75f));
    CHECK(a.track_gain(tb) == doctest::Approx(0.25f));
    a.update(0.5f);
    CHECK(a.track_gain(ta) == doctest::Approx(1.0f));
    CHECK(a.track_gain(tb) == 0.0f);
}

TEST_CASE("music volume is fade x music bus x master x duck; set_music_duck fades the duck") {
    Fixture f;
    Audio &a = f.audio();
    const auto ta = f.track("a.wav");
    a.play_music(ta, 0.0f);
    const std::uint32_t id = last_stream(f.device());
    a.update(0.0f);
    CHECK(stream_volume(f.device(), id) == doctest::Approx(1.0f));
    a.set_bus_volume(a.bus("music"), 0.5f);
    CHECK(stream_volume(f.device(), id) == doctest::Approx(0.5f));
    a.set_bus_volume(0, 0.5f);
    CHECK(stream_volume(f.device(), id) == doctest::Approx(0.25f));
    a.set_bus_volume(a.bus("sfx"), 0.1f); // another bus: no change
    CHECK(stream_volume(f.device(), id) == doctest::Approx(0.25f));

    a.set_music_duck(0.3f, 0.4f);
    a.update(0.2f);
    CHECK(a.music_duck() == doctest::Approx(0.65f));
    CHECK(stream_volume(f.device(), id) == doctest::Approx(0.25f * 0.65f));
    a.update(0.2f);
    CHECK(a.music_duck() == doctest::Approx(0.3f));
    a.set_music_duck(1.0f, 0.0f);
    CHECK(a.music_duck() == 1.0f);
    a.set_muted(true);
    CHECK(stream_volume(f.device(), id) == 0.0f);
}

TEST_CASE("stingers duck the music while they play; the music comes back after") {
    Fixture f;
    Audio &a = f.audio();
    const auto music = f.track("a.wav");
    const auto sting = f.track("sting.wav", 0.5f);
    a.play_music(music, 0.0f);
    const std::uint32_t music_id = last_stream(f.device());
    a.update(0.0f);
    a.play_stinger(sting, 0.3f);
    const std::uint32_t sting_id = last_stream(f.device());
    REQUIRE(sting_id != music_id);
    CHECK(last_of(f.device(), Kind::StreamPlay).slot == 0); // one-shot
    CHECK(a.track_gain(sting) == 1.0f);
    a.update(0.125f);
    CHECK(a.music_duck() == doctest::Approx(0.65f));
    a.update(0.125f);
    CHECK(a.music_duck() == doctest::Approx(0.3f));
    CHECK(stream_volume(f.device(), music_id) == doctest::Approx(0.3f));
    CHECK(stream_volume(f.device(), sting_id) == doctest::Approx(1.0f)); // not ducked itself

    f.device().finish_stream(sting_id); // the device reports its end
    a.update(0.1f);
    CHECK(a.track_gain(sting) == 0.0f);
    CHECK(last_of(f.device(), Kind::CloseStream).id == sting_id);
    a.update(0.35f);
    CHECK(a.music_duck() == doctest::Approx(0.3f + 0.7f * 0.35f));
    a.update(1.0f);
    CHECK(a.music_duck() == doctest::Approx(1.0f));
    CHECK(stream_volume(f.device(), music_id) == doctest::Approx(1.0f));
}

TEST_CASE("stingers end after their length when the device cannot tell; a new one replaces the old") {
    Fixture f;
    Audio &a = f.audio();
    const auto s1 = f.track("s1.wav", 0.5f);
    const auto s2 = f.track("s2.wav", 0.5f);
    a.play_stinger(s1, 0.5f);
    const std::uint32_t id1 = last_stream(f.device());
    a.update(0.55f);
    CHECK(a.track_gain(s1) == 1.0f);
    a.update(0.1f); // length + margin
    CHECK(a.track_gain(s1) == 0.0f);
    CHECK(last_of(f.device(), Kind::CloseStream).id == id1);

    a.play_stinger(s1, 0.5f);
    const std::uint32_t id1b = last_stream(f.device());
    a.update(0.1f);
    a.play_stinger(s2, 0.5f); // s1 fades out in 0.2 s
    const std::uint32_t id2 = last_stream(f.device());
    a.update(0.1f);
    CHECK(a.track_gain(s1) == doctest::Approx(0.5f));
    a.update(0.1f);
    CHECK(a.track_gain(s1) == 0.0f);
    CHECK(last_of(f.device(), Kind::CloseStream).id == id1b);
    CHECK(a.track_gain(s2) == 1.0f);
    const std::size_t opened = f.device().count(Kind::OpenStream);
    a.play_stinger(s2, 0.5f); // the same stinger again restarts it in place
    CHECK(f.device().count(Kind::OpenStream) == opened);
    CHECK(last_of(f.device(), Kind::StreamPlay).id == id2);
    CHECK(last_of(f.device(), Kind::StreamStop).id == id2);
    a.play_stinger({}, 0.5f); // none: ignored
}

TEST_CASE("ambience: layers fade in and out independently on the ambience bus") {
    Fixture f;
    Audio &a = f.audio();
    const auto wind = f.track("wind.wav");
    const auto rain = f.track("rain.wav");
    a.set_ambience(wind, 0.6f, 1.0f);
    const std::uint32_t wind_id = last_stream(f.device());
    a.set_ambience(rain, 1.0f, 0.5f);
    a.update(0.5f);
    CHECK(a.track_gain(wind) == doctest::Approx(0.3f));
    CHECK(a.track_gain(rain) == doctest::Approx(1.0f));
    a.update(0.5f);
    CHECK(a.track_gain(wind) == doctest::Approx(0.6f));
    a.set_bus_volume(a.bus("ambience"), 0.5f);
    CHECK(stream_volume(f.device(), wind_id) == doctest::Approx(0.3f));
    a.set_bus_volume(a.bus("music"), 0.1f); // the music bus does not touch ambience
    CHECK(stream_volume(f.device(), wind_id) == doctest::Approx(0.3f));

    a.set_ambience(wind, 0.0f, 0.5f);
    a.update(0.25f);
    CHECK(a.track_gain(wind) == doctest::Approx(0.3f));
    a.update(0.25f);
    CHECK(a.track_gain(wind) == 0.0f);
    CHECK(last_of(f.device(), Kind::CloseStream).id == wind_id);

    const std::size_t opened = f.device().count(Kind::OpenStream);
    a.set_ambience(wind, 0.0f, 1.0f); // silencing a layer that is not playing opens nothing
    CHECK(f.device().count(Kind::OpenStream) == opened);
    a.silence_ambience(1.0f);
    a.update(1.0f);
    CHECK(a.track_gain(rain) == 0.0f);
}

TEST_CASE("suspend: streams are silenced and their fades frozen, then resume") {
    Fixture f;
    Audio &a = f.audio();
    const auto ta = f.track("a.wav");
    a.play_music(ta, 1.0f);
    const std::uint32_t id = last_stream(f.device());
    a.update(0.5f);
    a.set_suspended(true);
    CHECK(stream_volume(f.device(), id) == 0.0f);
    a.update(1.0f);
    CHECK(a.track_gain(ta) == doctest::Approx(0.5f));
    a.set_suspended(false);
    CHECK(stream_volume(f.device(), id) == doctest::Approx(0.5f));
    a.update(0.5f);
    CHECK(a.track_gain(ta) == doctest::Approx(1.0f));
}

TEST_CASE("tracks: unloading closes the stream; a reload reopens it; missing tracks warn") {
    Fixture f;
    Audio &a = f.audio();
    lam::AssetServer &assets = f.assets();
    f.wav("live.wav", 1.0f);
    auto r = assets.load_now<lam::MusicTrack>("live.wav");
    REQUIRE(r.ok());
    a.play_music(*r, 0.0f);
    const std::uint32_t first = last_stream(f.device());
    REQUIRE(assets.reload(*r).ok()); // the old value is unloaded: its stream closes
    CHECK(last_of(f.device(), Kind::CloseStream).id == first);
    a.update(0.0f);
    const std::uint32_t second = last_stream(f.device());
    CHECK(second != first);
    CHECK(last_of(f.device(), Kind::StreamPlay).id == second);

    lam::test::CaptureLog log;
    a.play_music(lam::AssetId<lam::MusicTrack>{999, 1}, 0.0f);
    a.update(0.0f);
    CHECK(log.contains("not loaded"));
    CHECK(a.track_gain(lam::AssetId<lam::MusicTrack>{999, 1}) == 0.0f);

    const lam::AssetScope scope = assets.begin_scope();
    f.wav("gone.wav", 1.0f);
    auto g = assets.load_now<lam::MusicTrack>("gone.wav", scope);
    REQUIRE(g.ok());
    a.set_ambience(*g, 1.0f, 0.0f);
    const std::uint32_t gone_id = last_stream(f.device());
    assets.end_scope(scope);
    assets.collect_garbage();
    CHECK(last_of(f.device(), Kind::CloseStream).id == gone_id);
    a.update(0.0f); // the record is dropped, not reopened
    CHECK(a.track_gain(*g) == 0.0f);
}

TEST_CASE("set_device: music, stingers and ambience reopen on the new device") {
    Fixture f;
    Audio &a = f.audio();
    const auto ta = f.track("a.wav");
    a.play_music(ta, 0.0f);
    a.update(0.0f);
    a.set_device(nullptr);
    CHECK(f.device().calls().empty());
    a.update(0.0f);
    CHECK(f.device().count(Kind::OpenStream) == 1);
    CHECK(last_of(f.device(), Kind::StreamPlay).slot == 1);
    CHECK(a.track_gain(ta) == 1.0f);
}

TEST_CASE("streams are bounded: requests beyond kMaxStreams are ignored with a warning") {
    Fixture f;
    Audio &a = f.audio();
    lam::test::CaptureLog log;
    for (std::uint32_t i = 0; i <= Audio::kMaxStreams; ++i) {
        const auto t = f.track("amb" + std::to_string(i) + ".wav", 0.1f);
        a.set_ambience(t, 1.0f, 0.0f);
    }
    CHECK(f.device().count(Kind::OpenStream) == Audio::kMaxStreams);
    CHECK(log.contains("streams"));
}
