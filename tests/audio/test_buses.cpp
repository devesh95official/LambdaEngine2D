// The bus tree, volumes, mute and the "audio.<bus>.volume" settings.
#include <limits>
#include <string>

#include <lam/audio/audio.h>
#include <lam/audio/audio_plugin.h>
#include <lam/core/settings.h>
#include <lam/test/support.h>

#include "audio_test_util.h"

using namespace audio_test;
using lam::BusId;

TEST_CASE("buses: the default tree is master with sfx, ui, music and ambience under it") {
    Fixture f;
    lam::Audio &a = f.audio();
    REQUIRE(a.bus_count() == 5);
    CHECK(a.bus_name(0) == "master");
    CHECK(a.bus("master") == 0);
    CHECK(a.bus("sfx") == 1);
    CHECK(a.bus("ui") == 2);
    CHECK(a.bus("music") == 3);
    CHECK(a.bus("ambience") == 4);
    for (BusId b = 1; b < 5; ++b) {
        CHECK(a.bus_parent(b) == 0);
        CHECK(a.bus_volume(b) == 1.0f);
        CHECK(a.bus_gain(b) == 1.0f);
    }
    CHECK(a.bus_name(200).empty());
    CHECK(a.bus_volume(200) == 0.0f);
}

TEST_CASE("buses: user buses nest and their gain is the product of the chain") {
    Fixture f;
    lam::Audio &a = f.audio();
    const BusId steps = a.add_bus("footsteps", a.bus("sfx"));
    CHECK(steps == 5);
    CHECK(a.add_bus("footsteps") == steps); // same name: the existing bus
    CHECK(a.bus_parent(steps) == a.bus("sfx"));
    const BusId stray = a.add_bus("stray", 77); // unknown parent: master
    CHECK(a.bus_parent(stray) == 0);
    a.set_bus_volume(0, 0.5f);
    a.set_bus_volume(a.bus("sfx"), 0.8f);
    a.set_bus_volume(steps, 0.5f);
    CHECK(a.bus_gain(steps) == doctest::Approx(0.2f));
    CHECK(a.bus_gain(a.bus("sfx")) == doctest::Approx(0.4f));
    CHECK(a.bus_gain(a.bus("music")) == doctest::Approx(0.5f));
    a.set_bus_volume(steps, 3.0f); // clamped
    CHECK(a.bus_volume(steps) == 1.0f);
    a.set_bus_volume(steps, -1.0f);
    CHECK(a.bus_volume(steps) == 0.0f);
    a.set_bus_volume(steps, std::numeric_limits<float>::quiet_NaN());
    CHECK(a.bus_volume(steps) == 0.0f);
}

TEST_CASE("buses: a playing voice follows bus volume changes at once") {
    Fixture f;
    lam::Audio &a = f.audio();
    const BusId steps = a.add_bus("footsteps", a.bus("sfx"));
    a.set_bus_volume(0, 0.5f);
    a.set_bus_volume(a.bus("sfx"), 0.8f);
    a.set_bus_volume(steps, 0.5f);
    const auto clip = f.clip("step.wav");
    const lam::VoiceHandle v = a.play(clip, {.bus = steps});
    REQUIRE(v);
    CHECK(last_of(f.device(), Kind::Play).volume == doctest::Approx(0.2f));
    a.set_bus_volume(0, 1.0f);
    CHECK(last_of(f.device(), Kind::SetSlot).volume == doctest::Approx(0.4f));
    // The default bus of a clip play is sfx.
    const lam::VoiceHandle w = a.play(f.clip("other.wav"));
    REQUIRE(w);
    CHECK(last_of(f.device(), Kind::Play).volume == doctest::Approx(0.8f));
}

TEST_CASE("buses: unknown bus names fall back to master with a warning") {
    Fixture f;
    lam::test::CaptureLog log;
    CHECK(f.audio().bus("nope") == 0);
    CHECK(log.contains("no bus named 'nope'"));
}

TEST_CASE("settings: bus volumes are written to and read from the SettingsStore") {
    Fixture f;
    lam::Audio &a = f.audio();
    lam::SettingsStore &s = f.app().res<lam::SettingsStore>();
    REQUIRE(s.defined("audio.master.volume"));
    REQUIRE(s.defined("audio.ambience.volume"));
    CHECK(s.get_float("audio.music.volume") == 1.0f);
    // Audio -> settings.
    a.set_bus_volume(a.bus("music"), 0.3f);
    CHECK(s.get_float("audio.music.volume") == doctest::Approx(0.3f));
    // settings -> Audio (a settings menu), picked up by the next update.
    s.set_float("audio.sfx.volume", 0.25f);
    CHECK(a.bus_volume(a.bus("sfx")) == 1.0f);
    a.update(0.0f);
    CHECK(a.bus_volume(a.bus("sfx")) == doctest::Approx(0.25f));
    // A user bus defines its key.
    const BusId voice = a.add_bus("voice");
    REQUIRE(s.defined("audio.voice.volume"));
    s.set_float("audio.voice.volume", 2.0f); // the schema clamps
    a.update(0.0f);
    CHECK(a.bus_volume(voice) == 1.0f);
    s.set_float("audio.voice.volume", 0.5f);
    f.h.frames(1); // the engine's audio_update syncs too
    CHECK(a.bus_volume(voice) == doctest::Approx(0.5f));
}

TEST_CASE("settings: values loaded before the buses exist are applied when they are defined") {
    lam::test::TempDir dir;
    lam::test::CaptureLog log; // the out-of-range music volume is reported
    write_text(dir.file("settings.json"), R"({"audio.master.volume": 0.5, "audio.custom.volume": 0.2,
                                             "audio.music.volume": 7})");
    lam::test::HeadlessApp h;
    lam::SettingsStore &s = h.app().res<lam::SettingsStore>();
    REQUIRE(s.load(dir.file("settings.json")).ok());
    lam::AssetsConfig c;
    c.root = dir.path();
    h.add<lam::AssetsPlugin>(c);
    h.add<lam::AudioPlugin>();
    lam::Audio &a = h.app().res<lam::Audio>();
    CHECK(a.bus_volume(0) == doctest::Approx(0.5f));
    CHECK(a.bus_volume(a.bus("music")) == 1.0f); // out of range: the default is kept
    const BusId custom = a.add_bus("custom");
    CHECK(a.bus_volume(custom) == doctest::Approx(0.2f));
    CHECK(a.bus_gain(custom) == doctest::Approx(0.1f));
}

TEST_CASE("mute: every voice goes to volume 0 and comes back") {
    Fixture f;
    lam::Audio &a = f.audio();
    const lam::VoiceHandle v = a.play(f.clip("a.wav"), {.volume = 0.6f});
    REQUIRE(v);
    a.set_muted(true);
    CHECK(a.muted());
    CHECK(last_of(f.device(), Kind::SetSlot).volume == 0.0f);
    CHECK(a.playing(v)); // keeps running, inaudible
    a.set_muted(false);
    CHECK(last_of(f.device(), Kind::SetSlot).volume == doctest::Approx(0.6f));
}
