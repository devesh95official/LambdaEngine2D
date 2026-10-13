// Buses, settings sync, voices and bank groups. Streams are in audio_streams.cpp.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/audio/device.h>
#include <lam/core/log.h>
#include <lam/core/settings.h>
#include <lam/math/scalar.h>

#include "detail/audio_impl.h"

namespace lam {

namespace {
constexpr std::string_view kMuteUnfocusedKey = "audio.mute_unfocused";
constexpr float kEpsilon = 1e-4f;

float finite_or(float v, float fallback) { return std::isfinite(v) ? v : fallback; }
float clamp01(float v) { return std::isfinite(v) ? clamp(v, 0.0f, 1.0f) : 0.0f; }
} // namespace

// ---- Impl: construction, buses, settings ------------------------------------------------------

Audio::Impl::Impl(AssetServer &a) : assets(a), device(std::make_unique<NullAudioDevice>()) {
    buses.reserve(kMaxBuses);
    bus_gain.fill(1.0f);
    duck.snap(1.0f);
    stinger_duck.snap(1.0f);
}

void Audio::Impl::recompute_buses() {
    // Parents always have smaller ids, so one pass in id order sees every parent first.
    for (std::size_t i = 0; i < buses.size(); ++i) {
        const float own = buses[i].volume;
        bus_gain[i] = i == 0 ? own : own * bus_gain[buses[i].parent];
    }
}

float Audio::Impl::chain_gain(BusId b) const {
    if (b >= buses.size()) return 0.0f;
    float g = 1.0f;
    for (BusId cur = b;;) {
        g *= buses[cur].volume;
        if (cur == 0) break;
        cur = buses[cur].parent;
    }
    return g;
}

void Audio::Impl::define_bus_setting(Bus &b) {
    if (settings == nullptr) return;
    if (!settings->defined(b.key)) settings->define_float(b.key, 1.0f, 0.0f, 1.0f);
    b.volume = clamp01(settings->get_float(b.key));
}

void Audio::Impl::sync_settings() {
    if (settings == nullptr || settings->revision() == settings_revision) return;
    settings_revision = settings->revision();
    bool changed = false;
    for (Bus &b : buses) {
        if (!settings->defined(b.key)) continue;
        const float v = clamp01(settings->get_float(b.key));
        if (v != b.volume) {
            b.volume = v;
            changed = true;
        }
    }
    if (settings->defined(kMuteUnfocusedKey)) mute_unfocused = settings->get_bool(kMuteUnfocusedKey);
    const bool fm = focus_lost && mute_unfocused;
    if (fm != focus_muted) {
        focus_muted = fm;
        changed = true;
    }
    if (changed) {
        recompute_buses();
        apply_all();
    }
}

bool Audio::Impl::warn_once(std::uint64_t key) {
    for (std::uint32_t i = 0; i < warned_count; ++i)
        if (warned[i] == key) return false;
    if (warned_count >= warned.size()) { // table full: log once, then stay quiet
        if (!warned_overflow) {
            warned_overflow = true;
            LAM_LOG_WARN("audio: more than {} distinct audio warnings; further ones are suppressed",
                         warned.size());
        }
        return false;
    }
    warned[warned_count++] = key;
    return true;
}

// ---- Impl: voices -------------------------------------------------------------------------------

Audio::Impl::Voice *Audio::Impl::live(VoiceHandle h) {
    if (h.index == 0 || h.index > kMaxVoices) return nullptr;
    Voice &v = voices[h.index - 1];
    return (v.active && v.generation == h.generation) ? &v : nullptr;
}

const Audio::Impl::Voice *Audio::Impl::live(VoiceHandle h) const {
    if (h.index == 0 || h.index > kMaxVoices) return nullptr;
    const Voice &v = voices[h.index - 1];
    return (v.active && v.generation == h.generation) ? &v : nullptr;
}

void Audio::Impl::voice_output(const Voice &v, float &volume, float &pitch, float &pan) const {
    float gain = v.base * v.volume * v.fade * bus_gain[v.bus] * gate();
    float p = v.pan;
    if (v.spatial) {
        const SpatialMix m = spatial_mix(listener, v.position, v.range > 0.0f ? v.range : listener_range);
        gain *= m.gain;
        p += m.pan;
    }
    volume = clamp01(gain);
    pan = std::isfinite(p) ? clamp(p, -1.0f, 1.0f) : 0.0f;
    const float r = v.pitch * v.jitter;
    pitch = std::isfinite(r) ? clamp(r, detail::kMinPitch, detail::kMaxPitch) : 1.0f;
}

void Audio::Impl::apply(std::uint32_t slot, bool force) {
    Voice &v = voices[slot];
    if (!v.active) return;
    float vol = 0, pitch = 1, pan = 0;
    voice_output(v, vol, pitch, pan);
    if (!force && std::fabs(vol - v.out_volume) <= kEpsilon && std::fabs(pitch - v.out_pitch) <= kEpsilon &&
        std::fabs(pan - v.out_pan) <= kEpsilon)
        return;
    device->set_slot(static_cast<int>(slot), vol, pitch, pan);
    v.out_volume = vol;
    v.out_pitch = pitch;
    v.out_pan = pan;
}

void Audio::Impl::apply_all() {
    for (std::uint32_t i = 0; i < kMaxVoices; ++i)
        if (voices[i].active) apply(i, false);
    for (Stream &s : streams)
        if (s.role != Stream::Role::Free && s.started) apply_stream(s, false);
}

void Audio::Impl::free_voice(std::uint32_t slot) {
    Voice &v = voices[slot];
    if (!v.active) return;
    const std::uint32_t generation = v.generation + 1;
    v = Voice{};
    v.generation = generation;
    v.retired = generation == std::numeric_limits<std::uint32_t>::max();
    --active;
}

void Audio::Impl::kill(std::uint32_t slot) {
    if (!voices[slot].active) return;
    device->stop(static_cast<int>(slot));
    free_voice(slot);
}

int Audio::Impl::acquire(StrId group, std::uint32_t max_voices) {
    // 1. The group's cap: steal its oldest voice, fading-out ones first.
    if (group && max_voices > 0) {
        std::uint32_t count = 0;
        int victim = -1;
        bool victim_stopping = false;
        for (std::uint32_t i = 0; i < kMaxVoices; ++i) {
            const Voice &v = voices[i];
            if (!v.active || v.group != group) continue;
            ++count;
            const bool better = victim < 0 || (v.stopping && !victim_stopping) ||
                                (v.stopping == victim_stopping && v.order < voices[victim].order);
            if (better) {
                victim = static_cast<int>(i);
                victim_stopping = v.stopping;
            }
        }
        if (count >= max_voices && victim >= 0) {
            kill(static_cast<std::uint32_t>(victim));
            if (!voices[victim].retired) return victim;
        }
    }
    // 2. A free slot under the global limit.
    for (std::uint32_t i = 0; i < voice_limit; ++i)
        if (!voices[i].active && !voices[i].retired) return static_cast<int>(i);
    // 3. Steal under the global limit: the oldest fading-out voice, else the oldest one-shot.
    int victim = -1;
    for (int pass = 0; pass < 2 && victim < 0; ++pass) {
        for (std::uint32_t i = 0; i < voice_limit; ++i) {
            const Voice &v = voices[i];
            if (!v.active) continue;
            const bool eligible = pass == 0 ? v.stopping : !v.loop;
            if (eligible && (victim < 0 || v.order < voices[victim].order)) victim = static_cast<int>(i);
        }
    }
    if (victim < 0) return -1;
    kill(static_cast<std::uint32_t>(victim));
    return voices[victim].retired ? -1 : victim;
}

VoiceHandle Audio::Impl::start(int slot, std::uint32_t sound, const PlayParams &p, StrId group, BusId bus,
                               float base, float jitter) {
    Voice &v = voices[static_cast<std::size_t>(slot)];
    const std::uint32_t generation = v.generation;
    v = Voice{};
    v.generation = generation;
    v.active = true;
    v.loop = p.loop;
    v.spatial = p.spatial;
    v.bus = bus < buses.size() ? bus : detail::kBusSfx;
    v.sound = sound;
    v.group = group;
    v.base = base;
    v.volume = std::max(0.0f, finite_or(p.volume, 0.0f));
    v.pitch = finite_or(p.pitch, 1.0f);
    v.jitter = jitter;
    v.pan = finite_or(p.pan, 0.0f);
    v.range = std::max(0.0f, finite_or(p.range, 0.0f));
    v.position = p.position;
    v.order = ++play_counter;
    ++active;
    float vol = 0, pitch = 1, pan = 0;
    voice_output(v, vol, pitch, pan);
    device->play(sound, slot, vol, pitch, pan);
    v.out_volume = vol;
    v.out_pitch = pitch;
    v.out_pan = pan;
    return VoiceHandle{static_cast<std::uint32_t>(slot) + 1, v.generation};
}

bool Audio::Impl::spatial_silent(const PlayParams &p) const {
    if (!p.spatial || p.loop) return false;
    const SpatialMix m = spatial_mix(listener, p.position, p.range > 0.0f ? p.range : listener_range);
    return !(m.gain > 0.0f);
}

bool Audio::Impl::clip_throttled(std::uint64_t clip) const {
    if (!(clip_interval > 0.0f)) return false;
    for (const RecentClip &r : recent)
        if (r.clip == clip && now - r.time < static_cast<double>(clip_interval)) return true;
    return false;
}

void Audio::Impl::note_clip(std::uint64_t clip) {
    recent[recent_next] = RecentClip{clip, now};
    recent_next = (recent_next + 1) % static_cast<std::uint32_t>(recent.size());
}

void Audio::Impl::update_voices(float dt) {
    for (std::uint32_t i = 0; i < kMaxVoices; ++i) {
        Voice &v = voices[i];
        if (!v.active) continue;
        if (v.stopping) {
            v.fade -= v.fade_rate * dt;
            if (!(v.fade > 0.0f)) {
                kill(i);
                continue;
            }
        }
        if (!device->slot_playing(static_cast<int>(i))) {
            if (v.loop && !v.stopping) {
                // The device interface has no looping sounds, so restart it.
                float vol = 0, pitch = 1, pan = 0;
                voice_output(v, vol, pitch, pan);
                device->play(v.sound, static_cast<int>(i), vol, pitch, pan);
                v.out_volume = vol;
                v.out_pitch = pitch;
                v.out_pan = pan;
                continue;
            }
            free_voice(i);
            continue;
        }
        apply(i, false);
    }
}

void Audio::Impl::shutdown() {
    for (std::uint32_t i = 0; i < kMaxVoices; ++i) kill(i);
    for (std::size_t i = 0; i < streams.size(); ++i)
        if (streams[i].role != Stream::Role::Free) free_stream(static_cast<int>(i));
    music = {};
    stinger = -1;
    stinger_duck.snap(1.0f);
}

// ---- Impl: banks and groups ---------------------------------------------------------------------

Audio::Impl::Group *Audio::Impl::find_group(StrId id) {
    auto it = std::lower_bound(groups.begin(), groups.end(), id.value,
                               [](const Group &g, std::uint64_t v) { return g.id.value < v; });
    return (it != groups.end() && it->id == id) ? &*it : nullptr;
}

const Audio::Impl::Group *Audio::Impl::find_group(StrId id) const {
    auto it = std::lower_bound(groups.begin(), groups.end(), id.value,
                               [](const Group &g, std::uint64_t v) { return g.id.value < v; });
    return (it != groups.end() && it->id == id) ? &*it : nullptr;
}

BusId Audio::Impl::resolve_bus(StrId name) {
    if (!name) return detail::kBusSfx;
    for (std::size_t i = 0; i < buses.size(); ++i)
        if (buses[i].id == name) return static_cast<BusId>(i);
    if (warn_once(name.value ^ 0xB005B005ULL))
        LAM_LOG_WARN("audio: unknown bus '{}' in a sound bank; using \"sfx\"", id_display(name));
    return detail::kBusSfx;
}

void Audio::Impl::refresh_banks() {
    bool changed = false;
    for (BankRef &b : banks) {
        const LoadState st = assets.state(b.id);
        const bool ready = st == LoadState::Ready;
        const std::uint32_t version = ready ? assets.version(b.id) : 0;
        if (ready != b.ready || version != b.version) {
            b.ready = ready;
            b.version = version;
            changed = true;
        }
        if (st == LoadState::Failed && !b.failed_warned) {
            b.failed_warned = true;
            LAM_LOG_WARN("audio: sound bank '{}' failed to load; its groups are silent",
                         assets.path_of(b.id));
        }
    }
    if (changed) rebuild_groups();
}

bool Audio::Impl::banks_loading() const {
    for (const BankRef &b : banks) {
        const LoadState st = assets.state(b.id);
        if (st == LoadState::Queued || st == LoadState::Loading) return true;
    }
    return false;
}

void Audio::Impl::rebuild_groups() {
    std::vector<Group> fresh;
    for (const BankRef &b : banks) {
        if (!b.ready) continue;
        const SoundBank *bank = assets.get(b.id);
        if (bank == nullptr) continue;
        for (std::size_t j = 0; j < bank->groups.size(); ++j) {
            const SoundBank::Group &def = bank->groups[j];
            if (!def.id) continue;
            Group g;
            g.id = def.id;
            g.bank = b.id;
            g.index = static_cast<std::uint32_t>(j);
            g.bus = resolve_bus(def.bus);
            if (const Group *old = find_group(def.id)) {
                g.last_variant = old->last_variant;
                g.last_play = old->last_play;
            }
            auto dup = std::find_if(fresh.begin(), fresh.end(), [&](const Group &f) { return f.id == g.id; });
            if (dup != fresh.end()) {
                LAM_LOG_WARN("audio: sound group '{}' is defined twice; the later bank wins", id_display(g.id));
                *dup = g;
            } else {
                fresh.push_back(g);
            }
        }
    }
    std::sort(fresh.begin(), fresh.end(), [](const Group &a, const Group &b) { return a.id.value < b.id.value; });
    groups = std::move(fresh);
}

// ---- Audio: construction and device ------------------------------------------------------------

Audio::Audio(AssetServer &assets) : impl_(std::make_unique<Impl>(assets)) {
    add_bus("master");
    add_bus("sfx", detail::kBusMaster);
    add_bus("ui", detail::kBusMaster);
    add_bus("music", detail::kBusMaster);
    add_bus("ambience", detail::kBusMaster);
}

Audio::~Audio() = default;

void Audio::set_device(std::unique_ptr<AudioDevice> device) {
    Impl &m = *impl_;
    for (std::uint32_t i = 0; i < kMaxVoices; ++i) m.kill(i);
    m.detach_streams();
    m.device = device ? std::move(device) : std::make_unique<NullAudioDevice>();
    ++m.device_epoch;
    if (m.suspended) m.device->set_paused(true);
}

AudioDevice &Audio::device() { return *impl_->device; }

// ---- Audio: buses -------------------------------------------------------------------------------

BusId Audio::add_bus(std::string_view name, BusId parent) {
    Impl &m = *impl_;
    LAM_ASSERT(!name.empty(), "Audio::add_bus: empty name");
    if (name.empty()) return detail::kBusMaster;
    for (std::size_t i = 0; i < m.buses.size(); ++i)
        if (m.buses[i].name == name) return static_cast<BusId>(i);
    LAM_ASSERT(m.buses.size() < kMaxBuses, "Audio::add_bus: more than Audio::kMaxBuses buses");
    if (m.buses.size() >= kMaxBuses) return detail::kBusMaster;
    Impl::Bus b;
    b.name = std::string(name);
    b.id = intern(name);
    b.parent = (m.buses.empty() || parent >= m.buses.size()) ? detail::kBusMaster : parent;
    b.key = "audio." + b.name + ".volume";
    m.buses.push_back(std::move(b));
    m.define_bus_setting(m.buses.back());
    m.recompute_buses();
    return static_cast<BusId>(m.buses.size() - 1);
}

BusId Audio::bus(std::string_view name) const {
    const Impl &m = *impl_;
    for (std::size_t i = 0; i < m.buses.size(); ++i)
        if (m.buses[i].name == name) return static_cast<BusId>(i);
    LAM_LOG_WARN("audio: no bus named '{}'; using master", name);
    return detail::kBusMaster;
}

void Audio::set_bus_volume(BusId b, float volume) {
    Impl &m = *impl_;
    if (b >= m.buses.size()) return;
    m.sync_settings(); // apply changes made elsewhere first: settings_revision skips them below
    const float v = clamp01(volume);
    m.buses[b].volume = v;
    if (m.settings != nullptr && m.settings->defined(m.buses[b].key)) {
        m.settings->set_float(m.buses[b].key, v);
        m.settings_revision = m.settings->revision();
    }
    m.recompute_buses();
    m.apply_all();
}

float Audio::bus_volume(BusId b) const {
    const Impl &m = *impl_;
    return b < m.buses.size() ? m.buses[b].volume : 0.0f;
}

std::uint32_t Audio::bus_count() const { return static_cast<std::uint32_t>(impl_->buses.size()); }

std::string_view Audio::bus_name(BusId b) const {
    const Impl &m = *impl_;
    return b < m.buses.size() ? std::string_view(m.buses[b].name) : std::string_view();
}

BusId Audio::bus_parent(BusId b) const {
    const Impl &m = *impl_;
    return b < m.buses.size() ? m.buses[b].parent : detail::kBusMaster;
}

float Audio::bus_gain(BusId b) const { return impl_->chain_gain(b); }

void Audio::attach_settings(SettingsStore *settings) {
    Impl &m = *impl_;
    m.settings = settings;
    if (settings == nullptr) return;
    for (Impl::Bus &b : m.buses) m.define_bus_setting(b);
    if (!settings->defined(kMuteUnfocusedKey)) settings->define_bool(kMuteUnfocusedKey, false);
    m.mute_unfocused = settings->get_bool(kMuteUnfocusedKey);
    m.focus_muted = m.focus_lost && m.mute_unfocused;
    m.settings_revision = settings->revision();
    m.recompute_buses();
    m.apply_all();
}

// ---- Audio: gates and listener ------------------------------------------------------------------

void Audio::set_muted(bool muted) {
    Impl &m = *impl_;
    if (m.muted == muted) return;
    m.muted = muted;
    m.apply_all();
}

bool Audio::muted() const { return impl_->muted; }

void Audio::set_suspended(bool suspended) {
    Impl &m = *impl_;
    if (m.suspended == suspended) return;
    m.suspended = suspended;
    m.device->set_paused(suspended);
    m.apply_all();
}

bool Audio::suspended() const { return impl_->suspended; }

void Audio::set_listener(Vec2 position, float range) {
    Impl &m = *impl_;
    if (std::isfinite(position.x) && std::isfinite(position.y)) m.listener = position;
    if (range > 0.0f && std::isfinite(range)) m.listener_range = range;
}

// ---- Audio: playing -----------------------------------------------------------------------------

VoiceHandle Audio::play(AssetId<SoundClip> clip, const PlayParams &p) {
    Impl &m = *impl_;
    if (m.suspended) return {};
    const SoundClip *c = m.assets.get(clip);
    if (c == nullptr) return {};
    if (m.clip_throttled(clip.bits())) return {};
    if (m.spatial_silent(p)) return {};
    const std::uint32_t sound = c->device_epoch == m.device_epoch ? c->device_id : 0;
    int slot = -1;
    if (sound != 0) {
        slot = m.acquire({}, 0);
        if (slot < 0) return {};
    }
    m.note_clip(clip.bits());
    ++m.plays;
    if (sound == 0) return {};
    const BusId bus = p.bus != 0 ? p.bus : detail::kBusSfx;
    return m.start(slot, sound, p, {}, bus, 1.0f, 1.0f);
}

VoiceHandle Audio::play(StrId group, const PlayParams &p) {
    Impl &m = *impl_;
    if (m.suspended) return {};
    // A bank can finish loading after the last update(); pick it up now so this play isn't lost.
    m.refresh_banks();
    Impl::Group *g = m.find_group(group);
    if (g == nullptr) {
        // Don't warn while a bank is still loading, emitters ask for their group every frame.
        if (group && !m.banks_loading() && m.warn_once(group.value))
            LAM_LOG_WARN("audio: no sound group '{}' (add_bank a bank that defines it)", id_display(group));
        return {};
    }
    const SoundBank *bank = m.assets.get(g->bank);
    if (bank == nullptr || g->index >= bank->groups.size()) return {};
    const SoundBank::Group &def = bank->groups[g->index];
    const std::size_t n = def.variants.size();
    if (n == 0) return {};
    if (m.now - g->last_play < static_cast<double>(def.min_interval_ms) / 1000.0) return {};
    if (m.spatial_silent(p)) return {};
    // Pick among the other variants so the last one never repeats.
    std::uint32_t variant = 0;
    if (n > 1) {
        const auto count = static_cast<std::uint32_t>(n);
        if (g->last_variant >= 0 && static_cast<std::uint32_t>(g->last_variant) < count) {
            variant = m.rng.below(count - 1);
            if (variant >= static_cast<std::uint32_t>(g->last_variant)) ++variant;
        } else {
            variant = m.rng.below(count);
        }
    }
    const SoundClip *c = m.assets.get(def.variants[variant]);
    const std::uint32_t sound = (c != nullptr && c->device_epoch == m.device_epoch) ? c->device_id : 0;
    int slot = -1;
    if (sound != 0) {
        slot = m.acquire(group, def.max_voices);
        if (slot < 0) return {};
    }
    const float j = def.pitch_jitter;
    const float jitter = j > 0.0f ? 1.0f + m.rng.range(-j, j) : 1.0f;
    g->last_variant = static_cast<int>(variant);
    g->last_play = m.now;
    ++m.plays;
    if (sound == 0) return {};
    const BusId bus = p.bus != 0 ? p.bus : g->bus;
    return m.start(slot, sound, p, group, bus, def.volume, jitter);
}

void Audio::stop(VoiceHandle v, float fade_seconds) {
    Impl &m = *impl_;
    Impl::Voice *voice = m.live(v);
    if (voice == nullptr) return;
    if (!(fade_seconds > 0.0f) || !std::isfinite(fade_seconds)) {
        m.kill(v.index - 1);
        return;
    }
    voice->stopping = true;
    voice->fade_rate = std::max(voice->fade, 1e-3f) / fade_seconds;
}

bool Audio::playing(VoiceHandle v) const { return impl_->live(v) != nullptr; }

void Audio::set_volume(VoiceHandle v, float volume) {
    Impl &m = *impl_;
    if (Impl::Voice *voice = m.live(v)) {
        voice->volume = std::max(0.0f, finite_or(volume, 0.0f));
        m.apply(v.index - 1, false);
    }
}

void Audio::set_pitch(VoiceHandle v, float pitch) {
    Impl &m = *impl_;
    if (Impl::Voice *voice = m.live(v)) {
        voice->pitch = finite_or(pitch, 1.0f);
        m.apply(v.index - 1, false);
    }
}

void Audio::set_pan(VoiceHandle v, float pan) {
    Impl &m = *impl_;
    if (Impl::Voice *voice = m.live(v)) {
        voice->pan = finite_or(pan, 0.0f);
        m.apply(v.index - 1, false);
    }
}

void Audio::set_position(VoiceHandle v, Vec2 position) {
    Impl &m = *impl_;
    Impl::Voice *voice = m.live(v);
    if (voice == nullptr || !voice->spatial) return;
    if (!std::isfinite(position.x) || !std::isfinite(position.y)) return;
    voice->position = position;
    m.apply(v.index - 1, false);
}

void Audio::add_bank(AssetId<SoundBank> bank) {
    Impl &m = *impl_;
    if (!bank) return;
    for (const Impl::BankRef &b : m.banks)
        if (b.id == bank) return;
    m.banks.push_back(Impl::BankRef{bank});
    m.refresh_banks();
}

bool Audio::has_group(StrId group) const { return impl_->find_group(group) != nullptr; }

void Audio::update(float dt) {
    Impl &m = *impl_;
    if (!(dt > 0.0f) || !std::isfinite(dt)) dt = 0.0f;
    m.now += static_cast<double>(dt);
    m.sync_settings();
    m.refresh_banks();
    if (m.suspended) return;
    m.duck.update(dt);
    m.stinger_duck.update(dt);
    m.update_voices(dt);
    m.update_streams(dt);
    m.device->update_streams();
}

std::uint32_t Audio::plays() const { return impl_->plays; }

std::uint32_t Audio::active_voices() const { return impl_->active; }

void Audio::set_seed(std::uint64_t seed) { impl_->rng.reseed(seed); }

void Audio::set_voice_limit(std::uint32_t n) { impl_->voice_limit = std::clamp<std::uint32_t>(n, 1, kMaxVoices); }

std::uint32_t Audio::voice_limit() const { return impl_->voice_limit; }

void Audio::set_clip_min_interval(float seconds) {
    impl_->clip_interval = std::isfinite(seconds) ? std::max(0.0f, seconds) : 0.0f;
}

void Audio::stop_all(float fade_seconds) {
    Impl &m = *impl_;
    for (std::uint32_t i = 0; i < kMaxVoices; ++i) {
        const Impl::Voice &v = m.voices[i];
        if (v.active) stop(VoiceHandle{i + 1, v.generation}, fade_seconds);
    }
    if (m.stinger >= 0) {
        m.streams[static_cast<std::size_t>(m.stinger)].fade.to(0.0f, std::max(fade_seconds, 0.0f));
        m.stinger_duck.to(1.0f, detail::kStingerRelease);
        m.stinger = -1;
    }
}

double Audio::real_time() const { return impl_->now; }

// ---- AudioAccess (voices) -----------------------------------------------------------------------

namespace detail {

Status AudioAccess::upload_clip(Audio &a, SoundClip &clip) {
    Audio::Impl &m = *a.impl_;
    clip.device_epoch = m.device_epoch;
    if (!m.device->ready()) {
        clip.device_id = 0; // silent: no audio hardware
    } else {
        const std::uint32_t id = m.device->load_sound(clip.encoded, clip.extension);
        if (id == 0)
            return make_error(ErrorCode::Device, "the audio device cannot decode this " +
                                                     (clip.extension.empty() ? std::string("sound") : clip.extension) +
                                                     " file");
        clip.device_id = id;
    }
    std::vector<std::byte>().swap(clip.encoded);
    return {};
}

void AudioAccess::release_clip(Audio &a, SoundClip &clip) {
    Audio::Impl &m = *a.impl_;
    if (clip.device_id == 0 || clip.device_epoch != m.device_epoch) {
        clip.device_id = 0;
        return;
    }
    for (std::uint32_t i = 0; i < Audio::kMaxVoices; ++i)
        if (m.voices[i].active && m.voices[i].sound == clip.device_id) m.kill(i);
    m.device->unload_sound(clip.device_id);
    clip.device_id = 0;
}

void AudioAccess::release_track(Audio &a, const MusicTrack &track) { a.impl_->release_track(&track); }

void AudioAccess::set_owner(Audio &a, VoiceHandle v, std::uint32_t world, Entity e) {
    if (Audio::Impl::Voice *voice = a.impl_->live(v)) {
        voice->owner_world = world;
        voice->owner = e;
    }
}

StrId AudioAccess::voice_group(const Audio &a, VoiceHandle v) {
    const Audio::Impl::Voice *voice = a.impl_->live(v);
    return voice != nullptr ? voice->group : StrId{};
}

bool AudioAccess::group_full(Audio &a, StrId group) {
    Audio::Impl &m = *a.impl_;
    m.refresh_banks();
    const Audio::Impl::Group *g = m.find_group(group);
    if (g == nullptr) return false;
    const SoundBank *bank = m.assets.get(g->bank);
    if (bank == nullptr || g->index >= bank->groups.size()) return false;
    const std::uint32_t cap = bank->groups[g->index].max_voices;
    if (cap == 0) return false;
    std::uint32_t count = 0;
    for (const Audio::Impl::Voice &v : m.voices)
        if (v.active && !v.stopping && v.group == group) ++count;
    return count >= cap;
}

void AudioAccess::stop_world(Audio &a, std::uint32_t world_id) {
    Audio::Impl &m = *a.impl_;
    if (world_id == 0) return;
    for (std::uint32_t i = 0; i < Audio::kMaxVoices; ++i)
        if (m.voices[i].active && m.voices[i].owner_world == world_id) m.kill(i);
}

void AudioAccess::set_focus_lost(Audio &a, bool lost) {
    Audio::Impl &m = *a.impl_;
    m.focus_lost = lost;
    const bool fm = lost && m.mute_unfocused;
    if (fm == m.focus_muted) return;
    m.focus_muted = fm;
    m.apply_all();
}

void AudioAccess::shutdown(Audio &a) { a.impl_->shutdown(); }

} // namespace detail
} // namespace lam
