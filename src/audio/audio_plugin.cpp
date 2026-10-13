// AudioPlugin with audio_world (listener, PlaySound, emitters) and audio_update (lifecycle,
// Audio::update).
#include <cstdint>

#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/audio/audio_plugin.h>
#include <lam/audio/components.h>
#include <lam/core/app.h>
#include <lam/core/assert.h>
#include <lam/core/platform.h>
#include <lam/core/settings.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/rng.h>

#include "detail/audio_impl.h"
#include "detail/loaders.h"

namespace lam {

namespace detail {

void AudioAccess::sweep_world(Audio &a, World &w) {
    Audio::Impl &m = *a.impl_;
    const std::uint32_t world_id = w.id();
    for (std::uint32_t i = 0; i < Audio::kMaxVoices; ++i) {
        const Audio::Impl::Voice &v = m.voices[i];
        if (!v.active || v.owner_world != world_id) continue;
        // Owned voices are loops. Stop them once the emitter no longer wants one, e.g. after a restore.
        const AudioEmitter *em = w.alive(v.owner) ? w.try_get<AudioEmitter>(v.owner) : nullptr;
        const AudioVoice *av =
            (em != nullptr && em->group && em->loop) ? w.try_get<AudioVoice>(v.owner) : nullptr;
        if (av == nullptr || !(av->voice == VoiceHandle{i + 1, v.generation})) m.kill(i);
    }
}

} // namespace detail

namespace {

// The world's listener: the first entity with AudioListener and Transform.
struct WorldListener {
    bool present = false;
    Vec2 position;
    float range = 240;
};

SpatialMix mix_at(const WorldListener &l, Vec2 source, float range) {
    if (!l.present) return {1.0f, 0.0f}; // no listener in this world: heard as is, centred
    return spatial_mix(l.position, source, range > 0.0f ? range : l.range);
}

void audio_world(Ctx &ctx) {
    Audio &audio = ctx.res<Audio>();
    World &w = ctx.world();

    WorldListener listener;
    for (auto [e, l, t] : w.view<AudioListener, Transform>()) {
        listener = {true, t.position, l.range};
        break;
    }
    if (listener.present && ctx.focused()) audio.set_listener(listener.position, listener.range);

    // PlaySound from tick systems: a one-shot mixed against this world's listener at play time.
    for (const PlaySound &e : ctx.read<PlaySound>()) {
        PlayParams p;
        p.volume = e.volume;
        if (e.spatial) {
            const SpatialMix m = mix_at(listener, e.position, 0.0f);
            if (!(m.gain > 0.0f)) continue;
            p.volume *= m.gain;
            p.pan = m.pan;
        }
        audio.play(e.group, p);
    }

    // Emitters. AudioVoice isn't saved, so after a restore it's re-acquired here.
    for (auto [e, em, t] : w.view<AudioEmitter, Transform>()) {
        if (!em.group) continue;
        const SpatialMix m = mix_at(listener, t.position, em.range);
        const float volume = em.volume * m.gain;
        AudioVoice *av = w.try_get<AudioVoice>(e);
        if (av == nullptr) {
            VoiceHandle h;
            if (m.gain > 0.0f) {
                // Loops wait for a free voice. Stealing would make emitters fight over voices
                // every frame.
                const PlayParams params{.volume = volume, .pan = m.pan, .loop = em.loop};
                if (!em.loop || !detail::AudioAccess::group_full(audio, em.group))
                    h = audio.play(em.group, params);
                if (em.loop) detail::AudioAccess::set_owner(audio, h, w.id(), e);
                // Bank still loading: try again next frame.
                if (!em.loop && !h && !audio.has_group(em.group)) continue;
            }
            w.add<AudioVoice>(e, AudioVoice{h}); // not a pool this view iterates
            continue;
        }
        const bool live = audio.playing(av->voice);
        if (!em.loop) { // one-shot: plays once per AudioVoice, then only follows its emitter
            if (live) {
                audio.set_volume(av->voice, volume);
                audio.set_pan(av->voice, m.pan);
            }
            continue;
        }
        if (live && detail::AudioAccess::voice_group(audio, av->voice) != em.group) {
            audio.stop(av->voice);
            av->voice = {};
        }
        if (!(m.gain > 0.0f)) { // out of range: free the voice, restart when back in range
            if (audio.playing(av->voice)) audio.stop(av->voice);
            av->voice = {};
        } else if (!audio.playing(av->voice)) {
            if (detail::AudioAccess::group_full(audio, em.group)) continue; // wait for a free voice
            av->voice = audio.play(em.group, PlayParams{.volume = volume, .pan = m.pan, .loop = true});
            detail::AudioAccess::set_owner(audio, av->voice, w.id(), e);
        } else {
            audio.set_volume(av->voice, volume);
            audio.set_pan(av->voice, m.pan);
        }
    }

    // AudioVoice left on entities whose AudioEmitter was removed; voices whose owner is gone.
    for (auto [e, av] : w.view<AudioVoice>(Exclude<AudioEmitter>{})) {
        (void)av;
        w.commands().remove<AudioVoice>(e);
    }
    detail::AudioAccess::sweep_world(audio, w);
}

void audio_update(Ctx &ctx) {
    Audio &audio = ctx.res<Audio>();
    for (const LifecycleChanged &e : ctx.read<LifecycleChanged>()) {
        switch (e.event) {
        case LifecycleEvent::Background: audio.set_suspended(true); break;
        case LifecycleEvent::Foreground: audio.set_suspended(false); break;
        case LifecycleEvent::FocusLost: detail::AudioAccess::set_focus_lost(audio, true); break;
        case LifecycleEvent::FocusGained: detail::AudioAccess::set_focus_lost(audio, false); break;
        }
    }
    audio.update(ctx.time().frame_dt);
}

} // namespace

void AudioPlugin::build(App &app) {
    app.require_plugin("lam.assets", name());
    AssetServer *assets = app.try_res<AssetServer>();
    LAM_CHECK(assets != nullptr, "AudioPlugin needs the AssetServer: add AssetsPlugin before AudioPlugin");
    Audio &audio = app.insert_resource<Audio>(*assets);
    audio.set_seed(derive_seed(app.seed(), str_id("lam.audio").value));
    if (SettingsStore *settings = app.try_res<SettingsStore>()) audio.attach_settings(settings);
    detail::register_audio_loaders(*assets);
    if (app.events().try_get<LifecycleChanged>() == nullptr) app.add_event<LifecycleChanged>(EventScope::App, 8);
    app.add_event<PlaySound>(EventScope::World, 64);
    app.register_component<AudioListener>();
    app.register_component<AudioEmitter>();
    app.register_component<AudioVoice>();
    app.add_system(SystemDesc{.stage = Stage::Update,
                              .fn = &audio_world,
                              .name = "audio_world",
                              .order = -900,
                              .scope = SystemScope::PerWorld});
    app.add_system(SystemDesc{.stage = Stage::Update,
                              .fn = &audio_update,
                              .name = "audio_update",
                              .order = 1000,
                              .scope = SystemScope::Once});
}

void AudioPlugin::shutdown_world(App &app, World &world) {
    if (Audio *audio = app.try_res<Audio>()) detail::AudioAccess::stop_world(*audio, world.id());
}

void AudioPlugin::shutdown(App &app) {
    if (Audio *audio = app.try_res<Audio>()) detail::AudioAccess::shutdown(*audio);
}

} // namespace lam
