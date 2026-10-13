// AudioPlugin in a running App: PlaySound, listeners, emitters, restore, lifecycle and shutdown.
#include <array>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <lam/audio/audio.h>
#include <lam/audio/audio_plugin.h>
#include <lam/audio/components.h>
#include <lam/core/platform.h>
#include <lam/core/settings.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/test/support.h>

#include "audio_test_util.h"

using namespace audio_test;
using namespace lam::literals;
using lam::Audio;
using lam::AudioEmitter;
using lam::AudioListener;
using lam::AudioVoice;
using lam::Entity;
using lam::PlaySound;
using lam::Transform;
using lam::World;

namespace {

constexpr const char *kBank = R"({"groups": {
  "coin": {"files": ["coin.wav"], "min_interval_ms": 0, "pitch_jitter": 0},
  "hum":  {"files": ["hum.wav"], "min_interval_ms": 0, "pitch_jitter": 0, "max_voices": 8}
}})";

void load_bank(Fixture &f) {
    f.wav("coin.wav");
    f.wav("hum.wav", 1.0f);
    f.bank("world.sounds.json", kBank);
}

Entity spawn_at(World &w, lam::Vec2 p) {
    const Entity e = w.create();
    w.add<Transform>(e, Transform{p});
    return e;
}

std::uint32_t hum_id(Fixture &f) {
    const lam::SoundBank *b = f.assets().get(f.assets().load<lam::SoundBank>("world.sounds.json"));
    for (const auto &g : b->groups)
        if (g.id == "hum"_id) return f.assets().get(g.variants[0])->device_id;
    return 0;
}

std::size_t plays_of_sound(const lam::NullAudioDevice &d, std::uint32_t sound) {
    std::size_t n = 0;
    for (const auto &c : d.calls())
        if (c.kind == Kind::Play && c.id == sound) ++n;
    return n;
}

// The schedule line of a system ("" if absent).
std::string schedule_line(lam::App &app, std::string_view name) {
    std::istringstream in(app.schedule_text());
    std::string line;
    while (std::getline(in, line))
        if (line.find(std::string(name) + " ") != std::string::npos) return line;
    return {};
}

} // namespace

TEST_CASE("plugin: systems at their slots, components and the PlaySound event registered") {
    Fixture f;
    const std::string world = schedule_line(f.app(), "audio_world");
    CHECK(world.find("Update") != std::string::npos);
    CHECK(world.find("PerWorld") != std::string::npos);
    CHECK(world.find("-900") != std::string::npos);
    CHECK(world.find("[lam.audio]") != std::string::npos);
    const std::string update = schedule_line(f.app(), "audio_update");
    CHECK(update.find("Update") != std::string::npos);
    CHECK(update.find("Once") != std::string::npos);
    CHECK(update.find("1000") != std::string::npos);
    auto &reg = lam::ComponentRegistry::global();
    CHECK(reg.info<AudioListener>().flags == (lam::kSerialize | lam::kSnapshot));
    CHECK(reg.info<AudioEmitter>().flags == (lam::kSerialize | lam::kSnapshot));
    CHECK(reg.info<AudioVoice>().flags == 0u); // runtime: never saved, pruned on restore
    CHECK(f.app().main_world().events().try_get<PlaySound>() != nullptr);
    CHECK(f.app().has_plugin("lam.audio"));
}

namespace {
void emit_at_tick_5(lam::Ctx &ctx) {
    if (ctx.world().tick() == 5) ctx.emit(PlaySound{.group = "coin"_id});
}
} // namespace

TEST_CASE("PlaySound from a tick system is played exactly once, in the same frame") {
    Fixture f;
    load_bank(f);
    f.app().add_system(lam::Stage::Simulate, &emit_at_tick_5, "test.emit");
    f.h.ticks(4);
    CHECK(f.audio().plays() == 0);
    f.h.ticks(1);
    CHECK(f.audio().plays() == 1);
    CHECK(f.device().count(Kind::Play) == 1);
    f.h.ticks(20);
    CHECK(f.audio().plays() == 1);
}

namespace {
struct SpatialShots {
    int frame = 0;
};
void emit_spatial(lam::Ctx &ctx) {
    if (ctx.world().tick() == 1) {
        ctx.emit(PlaySound{"coin"_id, {100, 0}, true, 0.5f});
        ctx.emit(PlaySound{"coin"_id, {900, 0}, true, 1.0f}); // beyond the listener's range
    }
}
} // namespace

TEST_CASE("PlaySound: spatial events are mixed against the world's listener") {
    Fixture f;
    load_bank(f);
    World &w = f.app().main_world();
    w.add<AudioListener>(spawn_at(w, {0, 0}), AudioListener{200});
    f.app().add_system(lam::Stage::Simulate, &emit_spatial, "test.emit_spatial");
    f.h.ticks(2);
    CHECK(f.audio().plays() == 1);
    const lam::SpatialMix m = lam::spatial_mix({0, 0}, {100, 0}, 200);
    const auto p = last_of(f.device(), Kind::Play);
    CHECK(p.volume == doctest::Approx(0.5f * m.gain));
    CHECK(p.pan == doctest::Approx(m.pan));
}

TEST_CASE("the focused world's AudioListener becomes the Audio listener") {
    Fixture f;
    const auto clip = f.clip("c.wav");
    World &w = f.app().main_world();
    w.add<AudioListener>(spawn_at(w, {1000, 0}), AudioListener{100});
    f.h.frames(1);
    REQUIRE(f.audio().play(clip, {.position = {1000, 0}, .spatial = true}));
    CHECK(last_of(f.device(), Kind::Play).volume == doctest::Approx(1.0f));
    f.audio().update(0.1f);
    CHECK_FALSE(f.audio().play(clip, {.position = {1150, 0}, .spatial = true})); // listener range 100
}

TEST_CASE("emitters: a looping emitter plays in range, follows distance and stops out of range") {
    Fixture f;
    load_bank(f);
    World &w = f.app().main_world();
    w.add<AudioListener>(spawn_at(w, {0, 0}), AudioListener{240});
    const Entity e = spawn_at(w, {10, 0});
    w.add<AudioEmitter>(e, AudioEmitter{"hum"_id, true, 220, 0.8f});
    f.h.frames(1);
    REQUIRE(w.has<AudioVoice>(e));
    const lam::VoiceHandle v = w.get<AudioVoice>(e).voice;
    CHECK(f.audio().playing(v));
    CHECK(last_of(f.device(), Kind::Play).volume == doctest::Approx(0.8f));

    w.get<Transform>(e).position = {150, 0};
    f.h.frames(1);
    CHECK(f.audio().playing(v));
    const lam::SpatialMix m = lam::spatial_mix({0, 0}, {150, 0}, 220);
    CHECK(last_of(f.device(), Kind::SetSlot).volume == doctest::Approx(0.8f * m.gain));
    CHECK(last_of(f.device(), Kind::SetSlot).pan == doctest::Approx(m.pan));

    w.get<Transform>(e).position = {1000, 0};
    f.h.frames(1);
    CHECK_FALSE(f.audio().playing(v));
    CHECK_FALSE(w.get<AudioVoice>(e).voice);
    CHECK(f.audio().active_voices() == 0);

    w.get<Transform>(e).position = {20, 0};
    f.h.frames(1);
    CHECK(f.audio().playing(w.get<AudioVoice>(e).voice));
    CHECK(plays_of_sound(f.device(), hum_id(f)) == 2);

    w.get<AudioEmitter>(e).group = "coin"_id; // a new group restarts the loop with it
    f.h.frames(1);
    CHECK(last_of(f.device(), Kind::Play).id != hum_id(f));
    CHECK(f.audio().active_voices() == 1);
}

TEST_CASE("emitters: destroying the entity or removing the emitter stops its loop") {
    Fixture f;
    load_bank(f);
    World &w = f.app().main_world();
    const Entity a = spawn_at(w, {0, 0}); // no listener: heard as is
    w.add<AudioEmitter>(a, AudioEmitter{"hum"_id, true});
    const Entity b = spawn_at(w, {0, 0});
    w.add<AudioEmitter>(b, AudioEmitter{"hum"_id, true});
    f.h.frames(1);
    CHECK(f.audio().active_voices() == 2);
    w.destroy(a);
    f.h.frames(1);
    CHECK(f.audio().active_voices() == 1);
    w.remove<AudioEmitter>(b);
    f.h.frames(1);
    CHECK(f.audio().active_voices() == 0);
    f.h.frames(1);
    CHECK_FALSE(w.has<AudioVoice>(b)); // the runtime component goes too
}

TEST_CASE("emitters: a one-shot emitter plays once and outlives its entity") {
    Fixture f;
    load_bank(f);
    World &w = f.app().main_world();
    const Entity e = spawn_at(w, {0, 0});
    w.add<AudioEmitter>(e, AudioEmitter{"coin"_id, false});
    f.h.frames(10);
    CHECK(f.audio().plays() == 1);
    CHECK(w.has<AudioVoice>(e));
    w.destroy(e);
    f.h.frames(1);
    CHECK(f.audio().active_voices() == 1); // an explosion's sound outlives the explosion
}

TEST_CASE("restore prunes AudioVoice: orphaned loops stop, restored emitters re-acquire a voice") {
    Fixture f;
    load_bank(f);
    World &w = f.app().main_world();
    lam::WorldSnapshot before;
    w.snapshot_into(before); // no emitter yet
    const Entity e = spawn_at(w, {0, 0});
    w.add<AudioEmitter>(e, AudioEmitter{"hum"_id, true});
    f.h.frames(2);
    REQUIRE(f.audio().active_voices() == 1);
    w.restore(before); // e never existed: its AudioVoice is pruned, its loop is orphaned
    CHECK_FALSE(w.alive(e));
    f.h.frames(1);
    CHECK(f.audio().active_voices() == 0);

    const Entity e2 = spawn_at(w, {0, 0});
    w.add<AudioEmitter>(e2, AudioEmitter{"hum"_id, true});
    f.h.frames(1);
    REQUIRE(f.audio().active_voices() == 1);
    lam::WorldSnapshot with_emitter;
    w.snapshot_into(with_emitter);
    w.destroy(e2);
    f.h.frames(1);
    CHECK(f.audio().active_voices() == 0);
    w.restore(with_emitter); // e2 is back (AudioEmitter is snapshotted), without an AudioVoice
    REQUIRE(w.alive(e2));
    CHECK_FALSE(w.has<AudioVoice>(e2));
    f.h.frames(1);
    CHECK(w.has<AudioVoice>(e2));
    CHECK(f.audio().playing(w.get<AudioVoice>(e2).voice));
    CHECK(f.audio().active_voices() == 1);
}

TEST_CASE("lifecycle: background suspends, foreground resumes; focus loss mutes only if asked") {
    Fixture f;
    const auto clip = f.clip("c.wav");
    lam::App &app = f.app();
    const lam::VoiceHandle v = f.audio().play(clip, {.volume = 0.5f, .loop = true});
    REQUIRE(v);
    auto send = [&](lam::LifecycleEvent e) {
        app.events().get<lam::LifecycleChanged>().emit(lam::LifecycleChanged{e});
        f.h.frames(1);
    };
    send(lam::LifecycleEvent::Background);
    CHECK(f.audio().suspended());
    CHECK(f.device().paused());
    send(lam::LifecycleEvent::Foreground);
    CHECK_FALSE(f.audio().suspended());
    CHECK(last_of(f.device(), Kind::SetSlot).volume == doctest::Approx(0.5f));

    send(lam::LifecycleEvent::FocusLost); // "audio.mute_unfocused" is off by default
    CHECK(last_of(f.device(), Kind::SetSlot).volume == doctest::Approx(0.5f));
    send(lam::LifecycleEvent::FocusGained);
    app.res<lam::SettingsStore>().set_bool("audio.mute_unfocused", true);
    send(lam::LifecycleEvent::FocusLost);
    CHECK(last_of(f.device(), Kind::SetSlot).volume == 0.0f);
    send(lam::LifecycleEvent::FocusGained);
    CHECK(last_of(f.device(), Kind::SetSlot).volume == doctest::Approx(0.5f));
    CHECK(f.audio().playing(v));
}

TEST_CASE("destroying a world stops the loops its emitters own") {
    Fixture f;
    load_bank(f);
    lam::App &app = f.app();
    World &level = app.create_world("level");
    const std::array<lam::WorldEntry, 2> stack{lam::WorldEntry{&app.main_world()}, lam::WorldEntry{&level}};
    app.set_world_stack(stack);
    const Entity e = spawn_at(level, {0, 0});
    level.add<AudioEmitter>(e, AudioEmitter{"hum"_id, true});
    f.h.frames(1);
    REQUIRE(f.audio().active_voices() == 1);
    const std::array<lam::WorldEntry, 1> main_only{lam::WorldEntry{&app.main_world()}};
    app.set_world_stack(main_only);
    f.h.frames(1);
    CHECK(f.audio().active_voices() == 1); // an invisible world keeps its loops
    app.destroy_world(level);
    CHECK(f.audio().active_voices() == 0);
}

namespace {
// Records the order of device events into a shared log that outlives the App.
class LoggingDevice final : public lam::AudioDevice {
public:
    explicit LoggingDevice(std::shared_ptr<std::vector<std::string>> log) : log_(std::move(log)) {}
    ~LoggingDevice() override { log_->push_back("destroyed"); }
    bool ready() const override { return true; }
    std::uint32_t load_sound(std::span<const std::byte>, std::string_view) override {
        log_->push_back("load");
        return 1;
    }
    void unload_sound(std::uint32_t) override { log_->push_back("unload"); }
    void play(std::uint32_t, int, float, float, float) override { playing_ = true; }
    void stop(int) override {
        playing_ = false;
        log_->push_back("stop");
    }
    void set_slot(int, float, float, float) override {}
    bool slot_playing(int) const override { return playing_; }
    std::uint32_t open_stream(const std::string &) override { return 0; }
    std::uint32_t open_stream_memory(std::span<const std::byte>, std::string_view) override {
        log_->push_back("open");
        return 7;
    }
    void close_stream(std::uint32_t) override { log_->push_back("close"); }
    void stream_play(std::uint32_t, bool) override {}
    void stream_stop(std::uint32_t) override {}
    void stream_volume(std::uint32_t, float) override {}
    void update_streams() override {}
private:
    std::shared_ptr<std::vector<std::string>> log_;
    bool playing_ = false;
};
} // namespace

TEST_CASE("shutdown: voices stop and streams close, clips unload, then the device is destroyed") {
    auto log = std::make_shared<std::vector<std::string>>();
    {
        Fixture f;
        f.audio().set_device(std::make_unique<LoggingDevice>(log));
        const auto clip = f.clip("c.wav");
        const auto track = f.track("m.wav");
        REQUIRE(f.audio().play(clip, {.loop = true}));
        f.audio().play_music(track, 0.0f);
        f.h.frames(2);
        log->clear();
    }
    REQUIRE(log->size() == 4);
    CHECK((*log)[0] == "stop");
    CHECK((*log)[1] == "close");
    CHECK((*log)[2] == "unload");
    CHECK((*log)[3] == "destroyed");
}

namespace {
void random_shots(lam::Ctx &ctx) {
    lam::Rng &rng = ctx.rng("game"_id);
    if (rng.chance(0.3f)) ctx.emit(PlaySound{"coin"_id, {rng.range(-100, 100), 0}, true});
    if (rng.chance(0.05f)) {
        World &w = ctx.world();
        w.add<Transform>(w.create(), Transform{{rng.range(0, 50), 0}});
    }
}
std::uint64_t run_sim(bool with_sounds) {
    Fixture f;
    if (with_sounds) load_bank(f);
    f.app().add_system(lam::Stage::Simulate, &random_shots, "test.random_shots");
    f.h.ticks(120);
    if (with_sounds) CHECK(f.audio().plays() > 10);
    return f.app().main_world().state_hash();
}
} // namespace

TEST_CASE("determinism: sounds never feed back into the simulation's state hash") {
    lam::test::CaptureLog log; // the run without a bank warns about the unknown group
    CHECK(run_sim(true) == run_sim(false));
}

namespace {
// A ready device without recording: one-shots end after 12 frames; nothing allocates.
class QuietDevice final : public lam::AudioDevice {
public:
    bool ready() const override { return true; }
    std::uint32_t load_sound(std::span<const std::byte>, std::string_view) override { return ++next_; }
    void unload_sound(std::uint32_t) override {}
    void play(std::uint32_t, int slot, float, float, float) override {
        left_[static_cast<std::size_t>(slot)] = 12;
        ++plays;
    }
    void stop(int slot) override { left_[static_cast<std::size_t>(slot)] = 0; }
    void set_slot(int, float, float, float) override { ++updates; }
    bool slot_playing(int slot) const override { return left_[static_cast<std::size_t>(slot)] > 0; }
    std::uint32_t open_stream(const std::string &) override { return 0; }
    std::uint32_t open_stream_memory(std::span<const std::byte>, std::string_view) override {
        ++opens;
        return ++next_;
    }
    void close_stream(std::uint32_t) override {}
    void stream_play(std::uint32_t, bool) override {}
    void stream_stop(std::uint32_t) override {}
    void stream_volume(std::uint32_t, float) override { ++updates; }
    void update_streams() override {
        for (int &l : left_)
            if (l > 0) --l;
    }
    std::uint64_t plays = 0, updates = 0, opens = 0;
private:
    std::array<int, 64> left_{};
    std::uint32_t next_ = 0;
};

struct Busy {
    lam::AssetId<lam::MusicTrack> a, b, sting, rain;
    lam::Entity listener;
    std::array<lam::Entity, 3> emitters{};
};
void busy_ticks(lam::Ctx &ctx) { // tick: move things, emit sounds
    Busy &b = ctx.res<Busy>();
    World &w = ctx.world();
    const auto t = static_cast<float>(ctx.world().tick());
    w.get<Transform>(b.listener).position = {static_cast<float>(static_cast<int>(t) % 300), 0};
    for (std::size_t i = 0; i < b.emitters.size(); ++i)
        w.get<Transform>(b.emitters[i]).position = {static_cast<float>((static_cast<int>(t) * (i + 1) * 3) % 600), 0};
    if (ctx.world().tick() % 7 == 0) ctx.emit(PlaySound{"coin"_id, {t, 0}, true});
}
void busy_frames(lam::Ctx &ctx) { // frame: music, stingers, ambience, duck, direct plays
    Busy &b = ctx.res<Busy>();
    Audio &audio = ctx.res<Audio>();
    const std::uint64_t frame = ctx.time().frame;
    if (frame % 100 == 0) audio.play_music((frame / 100) % 2 == 0 ? b.a : b.b, 0.5f);
    if (frame % 150 == 75) audio.play_stinger(b.sting, 0.4f);
    if (frame % 120 == 0) audio.set_ambience(b.rain, (frame / 120) % 2 == 0 ? 0.7f : 0.0f, 0.5f);
    if (frame % 90 == 45) audio.set_music_duck((frame / 90) % 2 == 0 ? 0.5f : 1.0f, 0.3f);
    if (frame % 11 == 0) {
        const lam::VoiceHandle v = audio.play("hum"_id, {.volume = 0.3f});
        audio.stop(v, 0.1f);
    }
}
} // namespace

// Loads the bank and tracks, spawns a listener and three looping emitters, and adds the busy
// systems (sounds every few ticks and frames, music changes, stingers, ambience, ducks).
void start_busy(Fixture &f) {
    load_bank(f);
    World &w = f.app().main_world();
    Busy busy;
    busy.a = f.track("a.wav", 1.0f);
    busy.b = f.track("b.wav", 1.0f);
    busy.sting = f.track("sting.wav", 0.5f);
    busy.rain = f.track("rain.wav", 1.0f);
    busy.listener = spawn_at(w, {0, 0});
    w.add<AudioListener>(busy.listener, AudioListener{240});
    for (Entity &e : busy.emitters) {
        e = spawn_at(w, {0, 0});
        w.add<AudioEmitter>(e, AudioEmitter{"hum"_id, true, 200, 0.7f});
    }
    f.app().insert_resource<Busy>(busy);
    f.app().add_system(lam::Stage::Simulate, &busy_ticks, "test.busy_ticks");
    f.app().add_system(lam::SystemDesc{.stage = lam::Stage::Update,
                                       .fn = &busy_frames,
                                       .name = "test.busy_frames",
                                       .scope = lam::SystemScope::Once});
}

TEST_CASE("steady state: playback, emitters, PlaySound, music and fades allocate nothing") {
    Fixture f;
    auto device = std::make_unique<QuietDevice>();
    QuietDevice &dev = *device;
    f.audio().set_device(std::move(device));
    start_busy(f);
    lam::test::run_ticks(f.app(), 240); // warm-up: every path runs at least once
    const std::uint64_t plays = dev.plays, updates = dev.updates, opens = dev.opens;
    CHECK_NO_ALLOC(lam::test::run_ticks(f.app(), 600));
    CHECK(dev.plays > plays + 50);
    CHECK(dev.updates > updates + 500);
    CHECK(dev.opens > opens + 5);
}

TEST_CASE("steady state on the NullAudioDevice (headless runs): recording allocates nothing") {
    Fixture f; // keeps the default NullAudioDevice, as every headless game and test does
    start_busy(f);
    lam::test::run_ticks(f.app(), 240);
    const std::size_t recorded = f.device().calls().size();
    CHECK_NO_ALLOC(lam::test::run_ticks(f.app(), 600));
    CHECK(f.device().calls().size() > recorded + 500);
    CHECK(f.device().calls().size() <= lam::NullAudioDevice::kMaxRecordedCalls);
}
