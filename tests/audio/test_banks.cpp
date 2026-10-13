// Sound banks (.sounds.json): format, variants, jitter, voice caps, throttles and reloads.
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <lam/audio/audio.h>
#include <lam/core/id.h>
#include <lam/test/support.h>

#include "audio_test_util.h"

using namespace audio_test;
using namespace lam::literals;
using lam::Audio;
using lam::VoiceHandle;

namespace {

constexpr const char *kBank = R"({
  "bus": "sfx",
  "groups": {
    "step": {"file": "step.wav", "variants": 3, "min_interval_ms": 0, "pitch_jitter": 0.1},
    "coin": {"files": ["coin.wav"], "volume": 0.5, "bus": "ui", "pitch_jitter": 0},
    "hit":  {"files": ["hit_a.wav", "hit_b.wav"], "max_voices": 2, "min_interval_ms": 0},
    "slow": {"files": ["coin.wav"], "min_interval_ms": 100}
  }
})";

lam::AssetId<lam::SoundBank> load_bank(Fixture &f) {
    for (const char *n : {"sounds/step_1.wav", "sounds/step_2.wav", "sounds/step_3.wav", "sounds/coin.wav",
                          "sounds/hit_a.wav", "sounds/hit_b.wav"})
        f.wav(n);
    return f.bank("sounds/sfx.sounds.json", kBank);
}

// (sound id, pitch) of every Play call.
std::vector<std::pair<std::uint32_t, float>> plays_of(const lam::NullAudioDevice &d) {
    std::vector<std::pair<std::uint32_t, float>> out;
    for (const auto &c : calls_of(d, Kind::Play)) out.emplace_back(c.id, c.pitch);
    return out;
}

} // namespace

TEST_CASE("banks: groups from files lists and <id>_<n> variants, with volumes and buses") {
    Fixture f;
    const auto id = load_bank(f);
    const lam::SoundBank *bank = f.assets().get(id);
    REQUIRE(bank != nullptr);
    REQUIRE(bank->groups.size() == 4);
    const lam::SoundBank::Group *step = nullptr;
    for (const auto &g : bank->groups)
        if (g.id == "step"_id) step = &g;
    REQUIRE(step != nullptr);
    REQUIRE(step->variants.size() == 3);
    CHECK(f.assets().path_of(step->variants[0]) == "sounds/step_1.wav");
    CHECK(f.assets().path_of(step->variants[2]) == "sounds/step_3.wav");
    CHECK(step->pitch_jitter == doctest::Approx(0.1f));
    CHECK(step->max_voices == 3);
    CHECK(step->bus == "sfx"_id);

    Audio &a = f.audio();
    CHECK(a.has_group("step"_id));
    CHECK(a.has_group("coin"_id));
    CHECK_FALSE(a.has_group("nope"_id));
    a.add_bank(id); // idempotent
    a.set_bus_volume(a.bus("ui"), 0.5f);
    const VoiceHandle v = a.play("coin"_id);
    REQUIRE(v);
    const auto p = last_of(f.device(), Kind::Play);
    CHECK(p.volume == doctest::Approx(0.25f)); // group 0.5 x ui 0.5
    CHECK(p.pitch == 1.0f);                     // no jitter
}

TEST_CASE("variants: never the same one twice in a row, and every one gets played") {
    Fixture f;
    load_bank(f);
    Audio &a = f.audio();
    for (int i = 0; i < 300; ++i) {
        REQUIRE(a.play("step"_id));
        a.update(0.001f);
    }
    const auto plays = plays_of(f.device());
    REQUIRE(plays.size() == 300);
    std::set<std::uint32_t> used;
    for (std::size_t i = 0; i < plays.size(); ++i) {
        used.insert(plays[i].first);
        if (i > 0) CHECK(plays[i].first != plays[i - 1].first);
    }
    CHECK(used.size() == 3);
}

TEST_CASE("variants and jitter are deterministic for a seed and vary across seeds") {
    auto run = [](std::uint64_t seed) {
        Fixture f;
        load_bank(f);
        f.audio().set_seed(seed);
        for (int i = 0; i < 40; ++i) {
            f.audio().play("step"_id);
            f.audio().update(0.001f);
        }
        return plays_of(f.device());
    };
    const auto a = run(42);
    const auto b = run(42);
    const auto c = run(43);
    CHECK(a == b);
    CHECK(a != c);
}

TEST_CASE("pitch jitter stays within the group's range and multiplies the requested pitch") {
    Fixture f;
    load_bank(f);
    Audio &a = f.audio();
    std::set<float> seen;
    for (int i = 0; i < 50; ++i) {
        a.play("step"_id);
        const float p = last_of(f.device(), Kind::Play).pitch;
        CHECK(p >= 0.9f);
        CHECK(p <= 1.1f);
        seen.insert(p);
        a.play("step"_id, {.pitch = 2.0f});
        const float q = last_of(f.device(), Kind::Play).pitch;
        CHECK(q >= 1.8f);
        CHECK(q <= 2.2f);
        a.update(0.001f);
    }
    CHECK(seen.size() > 10);
}

TEST_CASE("group voice cap: the group's oldest voice is stolen, fading ones first") {
    Fixture f;
    load_bank(f);
    Audio &a = f.audio();
    const VoiceHandle h0 = a.play("hit"_id);
    const VoiceHandle h1 = a.play("hit"_id);
    const VoiceHandle s0 = a.play("step"_id);
    REQUIRE(h0);
    REQUIRE(h1);
    REQUIRE(s0);
    const VoiceHandle h2 = a.play("hit"_id); // cap 2: h0 goes
    REQUIRE(h2);
    CHECK(h2.index == h0.index);
    CHECK_FALSE(a.playing(h0));
    CHECK(a.playing(h1));
    CHECK(a.playing(s0)); // other groups are untouched
    CHECK(a.active_voices() == 3);
    a.stop(h1, 1.0f);
    const VoiceHandle h3 = a.play("hit"_id); // the fading one goes first, although h2 is newer
    REQUIRE(h3);
    CHECK(h3.index == h1.index);
    CHECK(a.playing(h2));
}

TEST_CASE("group throttle: min_interval_ms of real time between plays") {
    Fixture f;
    load_bank(f);
    Audio &a = f.audio();
    CHECK(a.play("slow"_id));
    CHECK_FALSE(a.play("slow"_id));
    a.update(0.05f);
    CHECK_FALSE(a.play("slow"_id));
    a.update(0.051f);
    CHECK(a.play("slow"_id));
    CHECK(a.play("hit"_id)); // min_interval_ms 0
    CHECK(a.play("hit"_id));
    CHECK(a.plays() == 4);
}

TEST_CASE("PlayParams::bus overrides the group's bus") {
    Fixture f;
    load_bank(f);
    Audio &a = f.audio();
    a.set_bus_volume(a.bus("music"), 0.4f);
    REQUIRE(a.play("coin"_id, {.bus = a.bus("music")}));
    CHECK(last_of(f.device(), Kind::Play).volume == doctest::Approx(0.2f)); // 0.5 x 0.4
}

TEST_CASE("unknown groups are silent and warn once") {
    Fixture f;
    lam::test::CaptureLog log;
    (void)lam::intern("nope"); // names of interned ids appear in messages
    CHECK_FALSE(f.audio().play("nope"_id));
    CHECK_FALSE(f.audio().play("nope"_id));
    CHECK_FALSE(f.audio().play(lam::StrId{}));
    CHECK(log.count(lam::LogLevel::Warn) == 1);
    CHECK(log.contains("nope"));
    CHECK(f.audio().plays() == 0);
}

TEST_CASE("an unknown bus in a bank falls back to sfx with a warning") {
    Fixture f;
    f.wav("x.wav");
    lam::test::CaptureLog log;
    f.bank("odd.sounds.json", R"({"groups": {"x": {"files": ["x.wav"], "bus": "nonexistent"}}})");
    CHECK(log.contains("nonexistent"));
    Audio &a = f.audio();
    a.set_bus_volume(a.bus("sfx"), 0.5f);
    REQUIRE(a.play("x"_id));
    CHECK(last_of(f.device(), Kind::Play).volume == doctest::Approx(0.5f));
}

TEST_CASE("bank errors name the file and the problem; the bank fails as a whole") {
    Fixture f;
    f.wav("x.wav");
    lam::test::CaptureLog log;
    auto load = [&](const char *name, const char *json) {
        f.file(name, json);
        return f.assets().load_now<lam::SoundBank>(name);
    };
    auto typo = load("typo.sounds.json", R"({"groups": {"x": {"files": ["x.wav"], "volumee": 1}}})");
    REQUIRE_FALSE(typo.ok());
    CHECK(typo.error().message.find("volumee") != std::string::npos);
    CHECK(typo.error().message.find("typo.sounds.json") != std::string::npos);
    auto both = load("both.sounds.json", R"({"groups": {"x": {"files": ["x.wav"], "file": "x.wav"}}})");
    REQUIRE_FALSE(both.ok());
    CHECK(both.error().message.find("exactly one") != std::string::npos);
    CHECK_FALSE(load("none.sounds.json", R"({"groups": {"x": {"volume": 1}}})").ok());
    CHECK_FALSE(load("zero.sounds.json", R"({"groups": {"x": {"file": "x.wav", "variants": 0}}})").ok());
    CHECK_FALSE(load("range.sounds.json", R"({"groups": {"x": {"files": ["x.wav"], "max_voices": 99}}})").ok());
    CHECK_FALSE(load("empty_list.sounds.json", R"({"groups": {"x": {"files": []}}})").ok());
    auto missing = load("missing.sounds.json", R"({"groups": {"x": {"files": ["nope.wav"]}}})");
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.find("nope.wav") != std::string::npos);
    CHECK_FALSE(load("bad.sounds.json", "{ not json").ok());
    auto empty = load("empty.sounds.json", R"({"groups": {}})");
    REQUIRE(empty.ok());
    CHECK(f.assets().get(*empty)->groups.empty());
}

TEST_CASE("a failed bank warns once and its groups stay silent") {
    Fixture f;
    f.file("broken.sounds.json", "{");
    lam::test::CaptureLog log;
    const auto id = f.assets().load<lam::SoundBank>("broken.sounds.json");
    f.audio().add_bank(id);
    f.audio().update(0.0f);
    f.audio().update(0.0f);
    CHECK(log.contains("failed to load"));
    log.clear();
    f.audio().update(0.0f);
    CHECK_FALSE(log.contains("failed to load"));
}

TEST_CASE("a reloaded bank is re-indexed by the next update") {
    Fixture f;
    f.wav("x.wav");
    f.wav("y.wav");
    const auto id = f.bank("live.sounds.json", R"({"groups": {"x": {"files": ["x.wav"]}}})");
    Audio &a = f.audio();
    CHECK(a.has_group("x"_id));
    CHECK_FALSE(a.has_group("y"_id));
    f.file("live.sounds.json", R"({"groups": {"x": {"files": ["x.wav"]}, "y": {"files": ["y.wav"]}}})");
    REQUIRE(f.assets().reload(id).ok());
    a.update(0.0f);
    CHECK(a.has_group("y"_id));
    CHECK(a.play("y"_id));
}
