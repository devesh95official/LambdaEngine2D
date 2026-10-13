// audio_demo: lam_audio, scripted so it needs no input.
//   - Sound bank (assets/sounds/demo.sounds.json): "blip" has 3 variants (no immediate repeats)
//     with pitch jitter, "coin" plays on the ui bus, "hum" is a looping emitter.
//   - A tick system emits PlaySound; audio_world plays each one panned and attenuated from the
//     world's AudioListener.
//   - A looping AudioEmitter orbits the listener, drifting out of range (voice stops) and back.
//   - A frame-stage script crossfades music, plays a stinger that ducks it, fades ambience and
//     changes bus volumes, logging each step.
// Run: audio_demo (windowed, loops), --hidden --frames N (CI), --headless (one pass on
// NullAudioDevice, then prints what the device was asked to do). Assets: make_sounds.py.
#include <cstdint>
#include <cstdio>
#include <span>

#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/audio/components.h>
#include <lam/audio/device.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/log.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/scalar.h>
#include <lam/render/camera.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>

using namespace lam;
using namespace lam::literals;

namespace {

struct Demo {                                                   // App resource
    AssetId<MusicTrack> calm, tense, sting, wind;
    Entity emitter;
    double tour_start = 0;                                      // Audio::real_time() of this tour
    int step = 0;                                               // next director step
    int tours = 0;
    Vec2 last_blip;                                             // where the last PlaySound came from
};

constexpr float kOrbitSeconds = 6.0f;                           // one lap of the hum emitter
constexpr int kBlipEveryTicks = 40;

// Startup (main world, root asset scope): assets, the bank, the listener, the emitter, a camera.
void setup(Ctx &ctx) {
    AssetServer &assets = ctx.res<AssetServer>();
    Audio &audio = ctx.res<Audio>();
    Demo &demo = ctx.res<Demo>();
    audio.add_bank(assets.load<SoundBank>("sounds/demo.sounds.json"));
    demo.calm = assets.load<MusicTrack>("music/calm.wav");
    demo.tense = assets.load<MusicTrack>("music/tense.wav");
    demo.sting = assets.load<MusicTrack>("music/sting.wav");
    demo.wind = assets.load<MusicTrack>("ambience/wind.wav");

    World &w = ctx.world();
    const Entity listener = w.create();
    w.add<Transform>(listener);
    w.add<AudioListener>(listener, AudioListener{.range = 240});
    demo.emitter = w.create();
    w.add<Transform>(demo.emitter, Transform{.position = {120, 0}});
    w.add<AudioEmitter>(demo.emitter,
                        AudioEmitter{.group = "hum"_id, .loop = true, .range = 230, .volume = 0.8f});
    const Entity camera = w.create();
    w.add<Camera2D>(camera);                                    // centred on the listener (0, 0)
}

// Simulate (tick): the emitter orbits on a breathing radius (60..300 px, so it leaves its 230 px
// range and comes back), and every kBlipEveryTicks a PlaySound comes from a random direction.
void simulate(Ctx &ctx) {
    World &w = ctx.world();
    Demo &demo = ctx.res<Demo>();
    const float t = static_cast<float>(w.tick()) * ctx.time().tick_dt;
    if (Transform *tr = w.try_get<Transform>(demo.emitter)) {
        const float angle = kTau * t / kOrbitSeconds;
        const float radius = 180.0f + 120.0f * det_sin(angle * 0.37f);
        tr->position = {radius * det_cos(angle), radius * 0.5f * det_sin(angle)};
    }
    if (w.tick() % kBlipEveryTicks == 0) {
        const Vec2 dir = ctx.rng("demo.blip"_id).unit_vector();
        demo.last_blip = dir * ctx.rng("demo.blip"_id).range(20.0f, 200.0f);
        ctx.emit(PlaySound{.group = "blip"_id, .position = demo.last_blip, .spatial = true});
    }
}

// Update (frame, Once): the scripted tour. Frame code may call Audio directly.
void director(Ctx &ctx) {
    Audio &audio = ctx.res<Audio>();
    Demo &demo = ctx.res<Demo>();
    const double t = audio.real_time() - demo.tour_start;
    auto at = [&](double when) { return t >= when; };
    switch (demo.step) {
    case 0:
        LAM_LOG_INFO("[{:5.1f}s] music: calm fades in over 1 s", t);
        audio.play_music(demo.calm, 1.0f);
        break;
    case 1:
        if (!at(1.0)) return;
        LAM_LOG_INFO("[{:5.1f}s] ambience: wind fades in to 0.5 over 2 s", t);
        audio.set_ambience(demo.wind, 0.5f, 2.0f);
        break;
    case 2:
    case 3:
    case 4:
        if (!at(2.0 + 0.15 * (demo.step - 2))) return;
        LAM_LOG_INFO("[{:5.1f}s] ui: coin (ui bus, no jitter)", t);
        audio.play("coin"_id);
        break;
    case 5:
        if (!at(4.0)) return;
        LAM_LOG_INFO("[{:5.1f}s] music: crossfade calm -> tense over 2 s", t);
        audio.play_music(demo.tense, 2.0f);
        break;
    case 6:
        if (!at(7.0)) return;
        LAM_LOG_INFO("[{:5.1f}s] stinger: the music ducks to 0.25 while it plays", t);
        audio.play_stinger(demo.sting, 0.25f);
        break;
    case 7:
        if (!at(9.0)) return;
        LAM_LOG_INFO("[{:5.1f}s] bus sfx -> 0.3 (blips and the hum get quieter)", t);
        audio.set_bus_volume(audio.bus("sfx"), 0.3f);
        break;
    case 8:
        if (!at(10.5)) return;
        LAM_LOG_INFO("[{:5.1f}s] pause-menu duck: music to 0.3 over 0.4 s", t);
        audio.set_music_duck(0.3f);
        break;
    case 9:
        if (!at(12.0)) return;
        LAM_LOG_INFO("[{:5.1f}s] unduck; sfx back to 1; ambience fades out; music back to calm", t);
        audio.set_music_duck(1.0f);
        audio.set_bus_volume(audio.bus("sfx"), 1.0f);
        audio.set_ambience(demo.wind, 0.0f, 1.5f);
        audio.play_music(demo.calm, 1.5f);
        break;
    default: {
        if (!at(15.0)) return;
        ++demo.tours;
        LAM_LOG_INFO("tour {} done: {} plays, {} voices active", demo.tours, audio.plays(),
                     audio.active_voices());
        const LaunchOptions &opt = ctx.app().options();
        if (opt.headless && opt.frames == 0) ctx.app().quit(0);   // headless: one tour
        demo.tour_start = audio.real_time();
        demo.step = 1;                                          // the music is already playing
        return;
    }
    }
    ++demo.step;
}

// Render (frame): the listener, its range, the emitter and the last blip, if a renderer is there.
void draw(Ctx &ctx) {
    RenderQueue *queue = ctx.try_res<RenderQueue>();
    if (queue == nullptr) return;
    const Demo &demo = ctx.res<Demo>();
    const World &w = ctx.world();
    queue->circle({0, 0}, 240, Color::hex(0x283040FFu), kLayerWorld, -2);
    queue->circle({0, 0}, 6, kWhite, kLayerWorld, 0);
    if (const Transform *tr = w.try_get<Transform>(demo.emitter)) {
        const AudioVoice *voice = w.try_get<AudioVoice>(demo.emitter);
        const bool heard = voice != nullptr && ctx.res<Audio>().playing(voice->voice);
        queue->circle(tr->position, heard ? 9.0f : 5.0f, Color::hex(heard ? 0xFFAA3CFFu : 0x808080FFu),
                      kLayerWorld, 1);
    }
    queue->circle(demo.last_blip, 4, Color::hex(0x78C8FFFFu), kLayerWorld, 1);
}

} // namespace

int main(int argc, char **argv) {
    // No settings file: the demo changes bus volumes and shouldn't persist them.
    App app({.title = "audio_demo", .window_size = {960, 540}, .settings_file = ""}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.insert_resource<Demo>();
    app.add_startup(setup, "demo.setup");
    app.add_system(Stage::Simulate, simulate, "demo.simulate");
    app.add_system(SystemDesc{.stage = Stage::Update, .fn = director, .name = "demo.director",
                              .scope = SystemScope::Once});
    app.add_system(Stage::Render, draw, "demo.draw");
    const int code = app.run();

    Audio &audio = app.res<Audio>();
    std::printf("audio_demo: %.1f s, %u plays, %u voices active at exit\n", audio.real_time(), audio.plays(),
                audio.active_voices());
    if (const auto *null_device = dynamic_cast<const NullAudioDevice *>(&audio.device())) {
        using Kind = NullAudioDevice::Call::Kind;
        std::printf("NullAudioDevice: %zu sounds loaded, %zu slot plays, %zu streams opened, "
                    "%zu volume changes\n",
                    null_device->count(Kind::LoadSound), null_device->count(Kind::Play),
                    null_device->count(Kind::OpenStream), null_device->count(Kind::StreamVolume));
    }
    return code;
}
