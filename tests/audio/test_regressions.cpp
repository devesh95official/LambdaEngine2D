// lam_audio regressions. Each one failed before its fix.
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <lam/audio/audio.h>
#include <lam/audio/audio_plugin.h>
#include <lam/audio/components.h>
#include <lam/core/log.h>
#include <lam/core/settings.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/test/support.h>

#include "audio_test_util.h"

using namespace audio_test;
using namespace lam::literals;
using lam::Audio;
using lam::AudioEmitter;
using lam::AudioVoice;
using lam::Entity;
using lam::World;

namespace {

Entity emitter_at(World &w, lam::Vec2 p, AudioEmitter em) {
    const Entity e = w.create();
    w.add<lam::Transform>(e, lam::Transform{p});
    w.add<AudioEmitter>(e, em);
    return e;
}

std::uint32_t group_sound(Fixture &f, lam::AssetId<lam::SoundBank> bank, lam::StrId group) {
    const lam::SoundBank *b = f.assets().get(bank);
    REQUIRE(b != nullptr);
    for (const auto &g : b->groups)
        if (g.id == group) return f.assets().get(g.variants[0])->device_id;
    return 0;
}

void emit_start_at_tick_1(lam::Ctx &ctx) {
    if (ctx.world().tick() == 1) ctx.emit(lam::PlaySound{.group = "start"_id});
}

} // namespace

TEST_CASE("looping emitters beyond a group's max_voices wait instead of stealing") {
    Fixture f;
    f.wav("torch.wav", 5.0f);
    f.bank("fire.sounds.json", R"({"groups": {"torch": {"files": ["torch.wav"], "pitch_jitter": 0}}})"); // max_voices 3
    World &w = f.app().main_world();
    for (int i = 0; i < 4; ++i)
        emitter_at(w, {0, 0}, AudioEmitter{"torch"_id, true});
    f.h.frames(120);
    CHECK(f.device().count(Kind::Play) == 3);                  // was 60 (round-robin restarts)
    CHECK(f.device().count(Kind::Stop) == 0);                  // was 57
    CHECK(f.audio().active_voices() == 3);
}

TEST_CASE("a bank that becomes Ready this frame plays this frame's sounds") {
    // Asynchronous assets (a windowed game): the startup tick hold releases the first tick in the
    // frame whose asset_update finalized the bank, before audio_update re-indexes banks.
    lam::test::TempDir dir;
    write_bytes(dir.file("start.wav"), make_wav(0.25f));
    write_text(dir.file("boot.sounds.json"), R"({"groups": {"start": {"files": ["start.wav"], "min_interval_ms": 0}}})");
    lam::AppConfig config;
    config.settings_file = "";
    lam::App app(std::move(config));
    lam::AssetsConfig ac;
    ac.root = dir.path();
    ac.hot_reload = false;
    app.add_plugin<lam::AssetsPlugin>(ac);
    app.add_plugin<lam::AudioPlugin>();
    REQUIRE_FALSE(app.res<lam::AssetServer>().synchronous());
    Audio &audio = app.res<Audio>();
    audio.add_bank(app.res<lam::AssetServer>().load<lam::SoundBank>("boot.sounds.json"));
    World &w = app.main_world();
    const Entity e = emitter_at(w, {0, 0}, AudioEmitter{"start"_id, false}); // one-shot
    app.add_system(lam::Stage::Simulate, &emit_start_at_tick_1, "test.emit_start");
    for (int i = 0; i < 2000 && w.tick() < 1; ++i) {
        app.run_frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(w.tick() >= 1);
    app.run_frames(5);
    CHECK(audio.plays() == 2);                                  // was 0: both were lost
    REQUIRE(w.has<AudioVoice>(e));
}

TEST_CASE("a reloaded bank never plays a stale group index") {
    Fixture f;
    f.wav("a.wav");
    f.wav("b.wav");
    f.wav("c.wav");
    const auto id = f.bank("live.sounds.json", R"({"groups": {"a": {"files": ["a.wav"]},
        "c": {"files": ["c.wav"], "volume": 0.5, "pitch_jitter": 0}}})");
    f.file("live.sounds.json", R"({"groups": {"a": {"files": ["a.wav"]},
        "b": {"files": ["b.wav"], "volume": 1.0, "max_voices": 1, "pitch_jitter": 0},
        "c": {"files": ["c.wav"], "volume": 0.5, "pitch_jitter": 0}}})");
    REQUIRE(f.assets().reload(id).ok());
    REQUIRE(f.audio().play("c"_id));                            // before any update()
    const auto play = last_of(f.device(), Kind::Play);
    CHECK(play.id == group_sound(f, id, "c"_id));               // was b's sound
    CHECK(play.volume == doctest::Approx(0.5f));
}

TEST_CASE("clearing an emitter's group or loop flag stops its loop") {
    Fixture f;
    f.wav("hum.wav", 5.0f);
    f.bank("world.sounds.json", R"({"groups": {"hum": {"files": ["hum.wav"], "pitch_jitter": 0}}})");
    World &w = f.app().main_world();
    SUBCASE("group cleared") {
        const Entity e = emitter_at(w, {0, 0}, AudioEmitter{"hum"_id, true});
        f.h.frames(1);
        REQUIRE(f.audio().active_voices() == 1);
        w.get<AudioEmitter>(e).group = {};
        f.h.frames(2);
        CHECK(f.audio().active_voices() == 0);
        w.get<AudioEmitter>(e).group = "hum"_id; // wanted again: re-acquired
        f.h.frames(1);
        CHECK(f.audio().active_voices() == 1);
    }
    SUBCASE("loop turned off") {
        const Entity e = emitter_at(w, {0, 0}, AudioEmitter{"hum"_id, true});
        f.h.frames(1);
        REQUIRE(f.audio().active_voices() == 1);
        w.get<AudioEmitter>(e).loop = false;
        f.h.frames(2);
        CHECK(f.audio().active_voices() == 0);
    }
}

TEST_CASE("set_bus_volume keeps a settings change made earlier in the frame") {
    Fixture f;
    Audio &a = f.audio();
    lam::SettingsStore &s = f.app().res<lam::SettingsStore>();
    s.set_float("audio.master.volume", 0.2f);                  // a settings menu
    a.set_bus_volume(a.bus("sfx"), 0.8f);                       // game code, same frame
    a.update(0.0f);
    a.update(0.0f);
    CHECK(a.bus_volume(0) == doctest::Approx(0.2f));            // was 1.0
    CHECK(a.bus_volume(a.bus("sfx")) == doctest::Approx(0.8f));
}

TEST_CASE("once the warning table is full, warnings stop instead of repeating") {
    Fixture f;
    Audio &a = f.audio();
    lam::test::CaptureLog log;
    for (int i = 0; i < 32; ++i)
        (void)a.play(lam::intern("missing_" + std::to_string(i)));
    CHECK(log.count(lam::LogLevel::Warn) == 32);
    log.clear();
    const lam::StrId late = lam::intern("missing_late");
    for (int i = 0; i < 5; ++i)
        (void)a.play(late);
    CHECK(log.count(lam::LogLevel::Warn) == 1);                 // one notice (was 5 warnings)
    CHECK(log.contains("suppressed"));
    CHECK_FALSE(log.contains("missing_late"));
}

TEST_CASE("a stinger that gets no stream record releases the music duck") {
    Fixture f;
    Audio &a = f.audio();
    a.play_music(f.track("music.wav", 1.0f), 0.0f);
    a.play_stinger(f.track("s1.wav", 5.0f), 0.3f);
    for (int i = 0; i < 14; ++i)                                // every other stream record in use
        a.set_ambience(f.track("amb" + std::to_string(i) + ".wav", 1.0f), 1.0f, 0.0f);
    a.update(0.5f);
    REQUIRE(a.music_duck() == doctest::Approx(0.3f));
    lam::test::CaptureLog log;
    a.play_stinger(f.track("s2.wav", 1.0f), 0.3f);              // no record left: ignored
    CHECK(log.contains("streams"));
    for (int i = 0; i < 100; ++i)
        a.update(0.1f);
    CHECK(a.music_duck() == doctest::Approx(1.0f));             // was stuck at 0.3
}

TEST_CASE("no 'no sound group' warning while an added bank is still loading") {
    lam::test::TempDir dir;
    write_bytes(dir.file("x.wav"), make_wav(0.25f, 8000));
    write_text(dir.file("b.sounds.json"), R"({"groups": {"x": {"files": ["x.wav"]}}})");
    lam::AppConfig cfg;
    cfg.settings_file = "";
    lam::App app(cfg);                                          // not headless: asynchronous assets
    lam::AssetsConfig ac;
    ac.root = dir.path();
    app.add_plugin<lam::AssetsPlugin>(ac);
    app.add_plugin<lam::AudioPlugin>();
    lam::AssetServer &assets = app.res<lam::AssetServer>();
    REQUIRE_FALSE(assets.synchronous());
    Audio &audio = app.res<Audio>();
    lam::test::CaptureLog log;
    (void)lam::intern("x");
    (void)lam::intern("nope");
    audio.add_bank(assets.load<lam::SoundBank>("b.sounds.json")); // Queued: not decoded yet
    CHECK_FALSE(audio.play("x"_id));                            // not defined yet, but no warning
    CHECK(log.count(lam::LogLevel::Warn) == 0);
    assets.wait_all();
    CHECK(audio.play("x"_id));                                  // Ready: the group plays
    CHECK_FALSE(audio.play("nope"_id));                         // nothing pending can define it
    CHECK(log.count(lam::LogLevel::Warn) == 1);
    CHECK(log.contains("nope"));
}
