// Voices: handles, reaping, fades, limits, throttles, spatial plays and device swaps.
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

#include <lam/audio/audio.h>
#include <lam/audio/device.h>
#include <lam/core/app.h>
#include <lam/test/support.h>

#include "audio_test_util.h"

using namespace audio_test;
using lam::Audio;
using lam::PlayParams;
using lam::VoiceHandle;

namespace {

// A device without hardware: never ready, every call a no-op.
class DeadDevice final : public lam::AudioDevice {
public:
    bool ready() const override { return false; }
    std::uint32_t load_sound(std::span<const std::byte>, std::string_view) override { return 0; }
    void unload_sound(std::uint32_t) override {}
    void play(std::uint32_t, int, float, float, float) override {}
    void stop(int) override {}
    void set_slot(int, float, float, float) override {}
    bool slot_playing(int) const override { return false; }
    std::uint32_t open_stream(const std::string &) override { return 0; }
    void close_stream(std::uint32_t) override {}
    void stream_play(std::uint32_t, bool) override {}
    void stream_stop(std::uint32_t) override {}
    void stream_volume(std::uint32_t, float) override {}
    void update_streams() override {}
};

// A ready device that cannot decode anything.
class PickyDevice final : public lam::AudioDevice {
public:
    bool ready() const override { return true; }
    std::uint32_t load_sound(std::span<const std::byte>, std::string_view) override { return 0; }
    void unload_sound(std::uint32_t) override {}
    void play(std::uint32_t, int, float, float, float) override {}
    void stop(int) override {}
    void set_slot(int, float, float, float) override {}
    bool slot_playing(int) const override { return false; }
    std::uint32_t open_stream(const std::string &) override { return 0; }
    void close_stream(std::uint32_t) override {}
    void stream_play(std::uint32_t, bool) override {}
    void stream_stop(std::uint32_t) override {}
    void stream_volume(std::uint32_t, float) override {}
    void update_streams() override {}
};

std::uint32_t device_id(Fixture &f, lam::AssetId<lam::SoundClip> c) { return f.assets().get(c)->device_id; }

} // namespace

TEST_CASE("handles: play, playing, stop; stale handles are ignored and slots are reused") {
    Fixture f;
    Audio &a = f.audio();
    const auto clip = f.clip("a.wav");
    const VoiceHandle v = a.play(clip);
    REQUIRE(v);
    CHECK(v.index == 1);
    CHECK(v.generation == 1);
    CHECK(a.playing(v));
    CHECK(a.active_voices() == 1);
    CHECK(a.plays() == 1);
    const auto play = last_of(f.device(), Kind::Play);
    CHECK(play.slot == 0);
    CHECK(play.id == device_id(f, clip));

    a.stop(v);
    CHECK_FALSE(a.playing(v));
    CHECK(a.active_voices() == 0);
    CHECK(last_of(f.device(), Kind::Stop).slot == 0);

    const std::size_t n = f.device().calls().size();
    a.set_volume(v, 0.5f);
    a.set_pitch(v, 2.0f);
    a.set_pan(v, 1.0f);
    a.set_position(v, {1, 1});
    a.stop(v, 0.5f);
    a.stop(v);
    CHECK(f.device().calls().size() == n); // a stale handle never reaches the device

    a.update(0.1f); // past the clip throttle
    const VoiceHandle w = a.play(clip);
    REQUIRE(w);
    CHECK(w.index == 1);
    CHECK(w.generation == 2);
    CHECK_FALSE(v == w);
    CHECK_FALSE(a.playing(v));
    CHECK(a.playing(w));
    a.set_volume(v, 0.1f); // the old handle does not touch the new voice
    CHECK(f.device().calls().size() == n + 1);

    CHECK_FALSE(a.playing(VoiceHandle{}));
    CHECK_FALSE(a.playing(VoiceHandle{65, 1}));
    CHECK_FALSE(a.playing(VoiceHandle{1, 99}));
    CHECK_FALSE(a.play(lam::AssetId<lam::SoundClip>{}));
    CHECK_FALSE(a.play(lam::AssetId<lam::SoundClip>{77, 1}));
}

TEST_CASE("voices: finished one-shots are reaped; loops restart until stopped") {
    Fixture f;
    Audio &a = f.audio();
    a.set_clip_min_interval(0);
    const auto clip = f.clip("a.wav");
    const VoiceHandle v = a.play(clip);
    f.device().finish(0);
    CHECK(a.playing(v)); // until the next update
    a.update(0.016f);
    CHECK_FALSE(a.playing(v));
    CHECK(a.active_voices() == 0);

    const VoiceHandle l = a.play(clip, {.loop = true});
    const std::size_t plays_before = f.device().count(Kind::Play);
    f.device().finish(0);
    a.update(0.016f);
    CHECK(a.playing(l));
    CHECK(f.device().count(Kind::Play) == plays_before + 1);
    CHECK(last_of(f.device(), Kind::Play).slot == 0);
    a.stop(l);
    CHECK_FALSE(a.playing(l));
}

TEST_CASE("voices: stop with a fade ramps the volume down linearly, then stops") {
    Fixture f;
    Audio &a = f.audio();
    const VoiceHandle v = a.play(f.clip("a.wav"));
    a.stop(v, 0.5f);
    CHECK(a.playing(v));
    a.update(0.25f);
    CHECK(a.playing(v));
    CHECK(last_of(f.device(), Kind::SetSlot).volume == doctest::Approx(0.5f));
    a.update(0.3f);
    CHECK_FALSE(a.playing(v));
    CHECK(f.device().calls().back().kind == Kind::Stop);
}

TEST_CASE("voices: per-voice controls reach the device, clamped") {
    Fixture f;
    Audio &a = f.audio();
    const VoiceHandle v = a.play(f.clip("a.wav"), {.volume = 0.5f, .pitch = 1.5f, .pan = -0.25f});
    const auto p = last_of(f.device(), Kind::Play);
    CHECK(p.volume == doctest::Approx(0.5f));
    CHECK(p.pitch == doctest::Approx(1.5f));
    CHECK(p.pan == doctest::Approx(-0.25f));
    a.set_volume(v, 0.8f);
    CHECK(last_of(f.device(), Kind::SetSlot).volume == doctest::Approx(0.8f));
    a.set_pitch(v, 10.0f);
    CHECK(last_of(f.device(), Kind::SetSlot).pitch == doctest::Approx(4.0f));
    a.set_pitch(v, 0.0f);
    CHECK(last_of(f.device(), Kind::SetSlot).pitch == doctest::Approx(0.1f));
    a.set_pan(v, 3.0f);
    CHECK(last_of(f.device(), Kind::SetSlot).pan == doctest::Approx(1.0f));
    a.set_volume(v, 5.0f);
    CHECK(last_of(f.device(), Kind::SetSlot).volume == doctest::Approx(1.0f));
    a.set_volume(v, std::numeric_limits<float>::quiet_NaN());
    CHECK(last_of(f.device(), Kind::SetSlot).volume == 0.0f);
    // Unchanged values are not sent again.
    const std::size_t n = f.device().calls().size();
    a.set_pan(v, 1.0f);
    a.update(0.0f);
    CHECK(f.device().calls().size() == n);
}

TEST_CASE("voice limit: the oldest one-shot is stolen, fading voices first, loops are kept") {
    Fixture f;
    Audio &a = f.audio();
    a.set_clip_min_interval(0);
    a.set_voice_limit(4);
    CHECK(a.voice_limit() == 4);
    const auto c = f.clip("c.wav");
    const VoiceHandle h0 = a.play(c);
    const VoiceHandle h1 = a.play(c, {.loop = true});
    const VoiceHandle h2 = a.play(c);
    const VoiceHandle h3 = a.play(c);
    CHECK(a.active_voices() == 4);
    const VoiceHandle h4 = a.play(c); // full: the oldest one-shot (h0) goes, the older loop stays
    REQUIRE(h4);
    CHECK(h4.index == h0.index);
    CHECK_FALSE(a.playing(h0));
    CHECK(a.playing(h1));
    CHECK(a.active_voices() == 4);

    a.stop(h3, 1.0f); // fading out: the first to go, although h2 is older
    const VoiceHandle h5 = a.play(c);
    REQUIRE(h5);
    CHECK(h5.index == h3.index);
    CHECK_FALSE(a.playing(h3));
    CHECK(a.playing(h2));
    CHECK(a.playing(h4));

    a.set_voice_limit(0);
    CHECK(a.voice_limit() == 1);
    a.set_voice_limit(1000);
    CHECK(a.voice_limit() == Audio::kMaxVoices);
}

TEST_CASE("voice limit: when every voice loops, a new play is dropped and not counted") {
    Fixture f;
    Audio &a = f.audio();
    a.set_clip_min_interval(0);
    a.set_voice_limit(2);
    const auto c = f.clip("c.wav");
    CHECK(a.play(c, {.loop = true}));
    CHECK(a.play(c, {.loop = true}));
    CHECK(a.plays() == 2);
    CHECK_FALSE(a.play(c));
    CHECK(a.plays() == 2);
    CHECK(a.active_voices() == 2);
}

TEST_CASE("throttle: plays of the same clip closer than the interval of real time are dropped") {
    Fixture f;
    Audio &a = f.audio();
    const auto c = f.clip("c.wav");
    const auto d = f.clip("d.wav");
    CHECK(a.play(c));
    CHECK_FALSE(a.play(c)); // same instant
    CHECK(a.play(d));       // another clip is independent
    a.update(0.02f);
    CHECK_FALSE(a.play(c));
    a.update(0.011f);
    CHECK(a.play(c));
    CHECK(a.plays() == 3);
    CHECK(a.real_time() == doctest::Approx(0.031));
    a.set_clip_min_interval(0);
    CHECK(a.play(c));
    CHECK(a.play(c));
}

namespace {
struct ThrottleClip {
    lam::AssetId<lam::SoundClip> id;
};
void play_every_frame(lam::Ctx &ctx) { ctx.res<Audio>().play(ctx.res<ThrottleClip>().id); }
} // namespace

TEST_CASE("throttle: the engine advances real time every frame, so repeated plays are not all lost") {
    Fixture f;
    f.app().insert_resource<ThrottleClip>(ThrottleClip{f.clip("c.wav")});
    f.app().add_system(lam::SystemDesc{.stage = lam::Stage::Update,
                                       .fn = &play_every_frame,
                                       .name = "test.play_every_frame",
                                       .scope = lam::SystemScope::Once});
    f.h.frames(60); // 60 fps against a 30 ms interval: every other frame sounds
    CHECK(f.audio().plays() >= 29);
    CHECK(f.audio().plays() <= 31);
    CHECK(f.audio().real_time() == doctest::Approx(1.0).epsilon(0.03));
}

TEST_CASE("spatial: plays are attenuated and panned from the listener and follow set_position") {
    Fixture f;
    Audio &a = f.audio();
    a.set_clip_min_interval(0);
    const auto c = f.clip("c.wav");
    a.set_listener({0, 0}, 200);
    const VoiceHandle v = a.play(c, {.position = {100, 0}, .spatial = true});
    REQUIRE(v);
    const lam::SpatialMix m = lam::spatial_mix({0, 0}, {100, 0}, 200);
    CHECK(last_of(f.device(), Kind::Play).volume == doctest::Approx(m.gain));
    CHECK(last_of(f.device(), Kind::Play).pan == doctest::Approx(m.pan));
    a.set_position(v, {0, 25});
    CHECK(last_of(f.device(), Kind::SetSlot).volume == doctest::Approx(1.0f));
    CHECK(last_of(f.device(), Kind::SetSlot).pan == doctest::Approx(0.0f));
    a.set_listener({300, 0}, 200); // the listener walks away: silent next update
    a.update(0.0f);
    CHECK(last_of(f.device(), Kind::SetSlot).volume == 0.0f);
    CHECK(a.playing(v));

    const std::uint32_t plays = a.plays();
    CHECK_FALSE(a.play(c, {.position = {900, 0}, .spatial = true})); // out of range: not played
    CHECK(a.plays() == plays);
    const VoiceHandle far = a.play(c, {.position = {900, 0}, .spatial = true, .range = 1000});
    REQUIRE(far);
    CHECK(last_of(f.device(), Kind::Play).volume == doctest::Approx(lam::spatial_mix({300, 0}, {900, 0}, 1000).gain));
    const VoiceHandle loop = a.play(c, {.loop = true, .position = {5000, 0}, .spatial = true});
    REQUIRE(loop); // loops start silent out of range and fade in as the listener comes near
    CHECK(last_of(f.device(), Kind::Play).volume == 0.0f);

    const VoiceHandle flat = a.play(c);
    const std::size_t n = f.device().calls().size();
    a.set_position(flat, {1000, 1000}); // non-spatial voices ignore positions
    CHECK(f.device().calls().size() == n);
}

TEST_CASE("suspend: plays are dropped, voices are silenced and frozen, then restored") {
    Fixture f;
    Audio &a = f.audio();
    const auto c = f.clip("c.wav");
    const VoiceHandle v = a.play(c, {.volume = 0.7f});
    a.set_suspended(true);
    CHECK(a.suspended());
    CHECK(f.device().paused());
    CHECK(last_of(f.device(), Kind::SetSlot).volume == 0.0f);
    a.update(0.1f);
    CHECK_FALSE(a.play(c));
    f.device().finish(0); // a paused device reports nothing playing: not a reason to reap
    a.update(0.1f);
    CHECK(a.playing(v));
    a.set_suspended(false);
    CHECK_FALSE(f.device().paused());
    CHECK(last_of(f.device(), Kind::SetSlot).volume == doctest::Approx(0.7f));
    a.update(0.0f);
    CHECK_FALSE(a.playing(v)); // it had ended
}

TEST_CASE("no hardware: clips load silent, plays still count, handles are null") {
    Fixture f;
    Audio &a = f.audio();
    a.set_device(std::make_unique<DeadDevice>());
    const auto c = f.clip("c.wav");
    REQUIRE(f.assets().get(c) != nullptr);
    CHECK(f.assets().get(c)->device_id == 0);
    CHECK(f.assets().get(c)->encoded.empty()); // freed after "upload"
    CHECK_FALSE(a.play(c));
    CHECK(a.plays() == 1);
    CHECK(a.active_voices() == 0);
    a.update(0.1f);
}

TEST_CASE("a clip exists only after the device accepted it") {
    Fixture f;
    f.audio().set_device(std::make_unique<PickyDevice>());
    f.wav("c.wav");
    lam::test::CaptureLog log;
    auto r = f.assets().load_now<lam::SoundClip>("c.wav");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("cannot decode") != std::string::npos);
}

TEST_CASE("loaders: malformed or empty sound files fail with a readable error") {
    Fixture f;
    lam::test::CaptureLog log;
    f.file("bad.wav", "definitely not a wave file");
    auto bad = f.assets().load_now<lam::SoundClip>("bad.wav");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().message.find("bad.wav") != std::string::npos);
    f.file("empty.ogg", "");
    CHECK_FALSE(f.assets().load_now<lam::SoundClip>("empty.ogg").ok());
    CHECK_FALSE(f.assets().load_now<lam::MusicTrack>("empty.ogg").ok());
    CHECK_FALSE(f.assets().load_now<lam::SoundClip>("missing.wav").ok());
    const auto ok = f.clip("fine.wav", 0.5f);
    CHECK(f.assets().get(ok)->seconds == doctest::Approx(0.5f));
    CHECK(f.assets().get(ok)->extension == ".wav");
}

TEST_CASE("set_device: voices stop, clips of the old device fall silent, new loads play") {
    Fixture f;
    Audio &a = f.audio();
    a.set_clip_min_interval(0);
    const auto old_clip = f.clip("old.wav");
    const VoiceHandle v = a.play(old_clip);
    REQUIRE(v);
    a.set_device(nullptr); // a fresh NullAudioDevice
    CHECK_FALSE(a.playing(v));
    CHECK(f.device().calls().empty());
    CHECK_FALSE(a.play(old_clip)); // uploaded to the old device: silent, but counted
    CHECK(a.plays() == 2);
    const auto new_clip = f.clip("new.wav");
    CHECK(a.play(new_clip));
}

TEST_CASE("unloading a clip stops its voices and frees it on the device") {
    Fixture f;
    Audio &a = f.audio();
    lam::AssetServer &assets = f.assets();
    f.wav("tmp.wav");
    const lam::AssetScope scope = assets.begin_scope();
    auto r = assets.load_now<lam::SoundClip>("tmp.wav", scope);
    REQUIRE(r.ok());
    const std::uint32_t id = assets.get(*r)->device_id;
    const VoiceHandle v = a.play(*r, {.loop = true});
    REQUIRE(a.playing(v));
    assets.end_scope(scope);
    assets.collect_garbage();
    CHECK_FALSE(a.playing(v));
    CHECK(last_of(f.device(), Kind::UnloadSound).id == id);
    CHECK(last_of(f.device(), Kind::Stop).slot == static_cast<int>(v.index - 1));
}

TEST_CASE("NullAudioDevice: records calls, simulates playing state, bounds its memory") {
    lam::NullAudioDevice d;
    CHECK(d.ready());
    const std::byte b[1] = {std::byte{1}};
    CHECK(d.load_sound({}, ".wav") == 0);
    const std::uint32_t s = d.load_sound(b, ".wav");
    CHECK(s != 0);
    d.play(s, 3, 0.5f, 1.0f, 0.0f);
    CHECK(d.slot_playing(3));
    CHECK_FALSE(d.slot_playing(4));
    CHECK_FALSE(d.slot_playing(-1));
    CHECK_FALSE(d.slot_playing(64));
    d.stop(3);
    CHECK_FALSE(d.slot_playing(3));
    const std::uint32_t st = d.open_stream("/x.ogg");
    CHECK(d.open_stream("") == 0);
    d.stream_play(st, true);
    CHECK(d.stream_playing(st));
    CHECK(last_of(d, Kind::StreamPlay).slot == 1);
    d.finish_stream(st);
    CHECK_FALSE(d.stream_playing(st));
    d.stream_play(st, false);
    d.close_stream(st);
    CHECK_FALSE(d.stream_playing(st));
    CHECK(d.count(Kind::LoadSound) == 2);
    d.clear_calls();
    CHECK(d.calls().empty());
    lam::test::CaptureLog log;
    for (std::size_t i = 0; i < lam::NullAudioDevice::kMaxRecordedCalls + 10; ++i) d.set_slot(0, 0, 1, 0);
    CHECK(d.calls().size() == lam::NullAudioDevice::kMaxRecordedCalls);
    CHECK(log.count(lam::LogLevel::Warn) == 1);
}
