// Music, stingers and ambience. A stream opens once its MusicTrack is Ready and closes when
// it has faded out or finished.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/audio/device.h>
#include <lam/core/log.h>
#include <lam/math/scalar.h>

#include "detail/audio_impl.h"

namespace lam {

namespace {
float clamp01(float v) { return std::isfinite(v) ? clamp(v, 0.0f, 1.0f) : 0.0f; }
} // namespace

int Audio::Impl::find_stream(Stream::Role role, AssetId<MusicTrack> track) const {
    for (std::size_t i = 0; i < streams.size(); ++i)
        if (streams[i].role == role && streams[i].track == track) return static_cast<int>(i);
    return -1;
}

int Audio::Impl::alloc_stream(Stream::Role role, AssetId<MusicTrack> track, bool loop) {
    for (std::size_t i = 0; i < streams.size(); ++i) {
        Stream &s = streams[i];
        if (s.role != Impl::Stream::Role::Free) continue;
        s = Stream{};
        s.role = role;
        s.track = track;
        s.loop = loop;
        return static_cast<int>(i);
    }
    if (warn_once(0x5354524541ULL)) // "STREA"
        LAM_LOG_WARN("audio: more than {} music/stinger/ambience streams; request ignored", kMaxStreams);
    return -1;
}

void Audio::Impl::close_stream(Stream &s) {
    if (s.device_stream != 0) {
        device->stream_stop(s.device_stream);
        device->close_stream(s.device_stream);
    }
    s.device_stream = 0;
    s.source = nullptr;
    s.started = false;
    s.out_volume = -1;
}

void Audio::Impl::free_stream(int i) {
    Stream &s = streams[static_cast<std::size_t>(i)];
    close_stream(s);
    if (stinger == i) {
        stinger = -1;
        stinger_duck.to(1.0f, detail::kStingerRelease);
    }
    s = Stream{};
}

float Audio::Impl::stream_gain(const Stream &s) const {
    float g = s.fade.value * gate();
    switch (s.role) {
    case Impl::Stream::Role::Music: g *= bus_gain[detail::kBusMusic] * duck.value * stinger_duck.value; break;
    case Impl::Stream::Role::Stinger: g *= bus_gain[detail::kBusMusic] * duck.value; break;
    case Impl::Stream::Role::Ambience: g *= bus_gain[detail::kBusAmbience]; break;
    case Impl::Stream::Role::Free: return 0.0f;
    }
    return clamp01(g);
}

void Audio::Impl::apply_stream(Stream &s, bool force) {
    if (s.device_stream == 0) return;
    const float v = stream_gain(s);
    if (!force && std::fabs(v - s.out_volume) <= 1e-4f) return;
    device->stream_volume(s.device_stream, v);
    s.out_volume = v;
}

Audio::Impl::StartResult Audio::Impl::start_stream(int i) {
    Stream &s = streams[static_cast<std::size_t>(i)];
    const LoadState state = assets.state(s.track);
    if (state == LoadState::Queued || state == LoadState::Loading) return StartResult::Waiting;
    const MusicTrack *t = state == LoadState::Ready ? assets.get(s.track) : nullptr;
    if (t == nullptr) {
        if (warn_once(s.track.bits() ^ 0x7472616B00000000ULL))
            LAM_LOG_WARN("audio: music track '{}' is not loaded; it stays silent", assets.path_of(s.track));
        return StartResult::Failed;
    }
    std::uint32_t id = 0;
    if (!t->encoded.empty()) id = device->open_stream_memory(t->encoded, t->extension);
    if (id == 0 && !t->path.empty()) id = device->open_stream(t->path);
    if (id == 0 && device->ready() && warn_once(s.track.bits() ^ 0x6F70656E00000000ULL))
        LAM_LOG_WARN("audio: cannot open a stream on '{}'; it stays silent", t->path);
    s.device_stream = id;
    s.source = t;
    s.started = true;
    s.elapsed = 0;
    s.out_volume = -1;
    if (id != 0) {
        apply_stream(s, true); // volume first, so it never starts loud
        device->stream_play(id, s.loop);
    }
    if (s.role == Impl::Stream::Role::Stinger && stinger == i) stinger_duck.to(s.duck, detail::kStingerAttack);
    return StartResult::Started;
}

void Audio::Impl::try_start(int i) {
    if (suspended) return;
    Stream &s = streams[static_cast<std::size_t>(i)];
    if (s.started) return;
    if (start_stream(i) == StartResult::Failed) free_stream(i);
}

void Audio::Impl::update_streams(float dt) {
    for (std::size_t k = 0; k < streams.size(); ++k) {
        const int i = static_cast<int>(k);
        Stream &s = streams[k];
        if (s.role == Impl::Stream::Role::Free) continue;
        if (!s.started) {
            const StartResult r = start_stream(i);
            if (r == StartResult::Failed) {
                free_stream(i);
                continue;
            }
            if (r == StartResult::Waiting) continue;
        } else {
            s.fade.update(dt);
            s.elapsed += dt;
        }
        if (s.fade.value <= 0.0f && s.fade.target <= 0.0f) {
            free_stream(i);
            continue;
        }
        if (!s.loop) {
            const float length = s.source != nullptr ? s.source->seconds : 0.0f;
            bool ended = s.device_stream != 0 && !device->stream_playing(s.device_stream);
            if (length > 0.0f && s.elapsed >= length + detail::kStreamEndMargin) ended = true;
            if (s.device_stream == 0 && !(length > 0.0f)) ended = true; // silent, unknown length
            if (ended) {
                free_stream(i);
                continue;
            }
        }
        apply_stream(s, false);
    }
}

void Audio::Impl::release_track(const MusicTrack *track) {
    for (Stream &s : streams)
        if (s.role != Impl::Stream::Role::Free && s.source == track) close_stream(s); // reopens if it reloads
}

void Audio::Impl::detach_streams() {
    for (Stream &s : streams)
        if (s.role != Impl::Stream::Role::Free && s.started) close_stream(s); // reopened on the new device
}

// ---- Audio: music, stingers, ambience ------------------------------------------------------------

void Audio::play_music(AssetId<MusicTrack> track, float crossfade) {
    Impl &m = *impl_;
    if (track == m.music) return;
    m.music = track;
    for (Impl::Stream &s : m.streams)
        if (s.role == Impl::Stream::Role::Music && s.track != track) s.fade.to(0.0f, crossfade);
    if (!track) return;
    int i = m.find_stream(Impl::Stream::Role::Music, track);
    if (i < 0) {
        i = m.alloc_stream(Impl::Stream::Role::Music, track, true);
        if (i < 0) return;
        m.streams[static_cast<std::size_t>(i)].fade.snap(0.0f);
    }
    m.streams[static_cast<std::size_t>(i)].fade.to(1.0f, crossfade);
    m.try_start(i);
}

void Audio::play_stinger(AssetId<MusicTrack> track, float duck) {
    Impl &m = *impl_;
    if (!track) return;
    const float level = clamp01(duck);
    if (m.stinger >= 0) {
        const int old_index = m.stinger;
        Impl::Stream &old = m.streams[static_cast<std::size_t>(old_index)];
        if (old.track == track) { // the same stinger again: restart it (or keep waiting for it)
            old.duck = level;
            if (!old.started) return;
            if (old.device_stream != 0) {
                m.device->stream_stop(old.device_stream);
                m.device->stream_play(old.device_stream, false);
            }
            old.elapsed = 0;
            old.fade.snap(1.0f);
            m.stinger_duck.to(level, detail::kStingerAttack);
            return;
        }
        m.stinger = -1;
        if (old.started) {
            old.fade.to(0.0f, detail::kStingerReplaceFade);
        } else {
            m.free_stream(old_index); // never started: drop it
        }
    }
    const int i = m.alloc_stream(Impl::Stream::Role::Stinger, track, false);
    if (i < 0) { // no record: nothing owns the duck any more (a detached stinger never releases it)
        m.stinger_duck.to(1.0f, detail::kStingerRelease);
        return;
    }
    Impl::Stream &s = m.streams[static_cast<std::size_t>(i)];
    s.fade.snap(1.0f);
    s.duck = level;
    m.stinger = i;
    m.try_start(i);
}

void Audio::set_music_duck(float gain, float fade) { impl_->duck.to(clamp01(gain), fade); }

void Audio::set_ambience(AssetId<MusicTrack> loop, float gain, float fade) {
    Impl &m = *impl_;
    if (!loop) return;
    const float g = clamp01(gain);
    int i = m.find_stream(Impl::Stream::Role::Ambience, loop);
    if (i < 0) {
        if (g <= 0.0f) return;
        i = m.alloc_stream(Impl::Stream::Role::Ambience, loop, true);
        if (i < 0) return;
        m.streams[static_cast<std::size_t>(i)].fade.snap(0.0f);
    }
    m.streams[static_cast<std::size_t>(i)].fade.to(g, fade);
    m.try_start(i);
}

void Audio::silence_ambience(float fade) {
    for (Impl::Stream &s : impl_->streams)
        if (s.role == Impl::Stream::Role::Ambience) s.fade.to(0.0f, fade);
}

AssetId<MusicTrack> Audio::music() const { return impl_->music; }

float Audio::track_gain(AssetId<MusicTrack> track) const {
    float g = 0.0f;
    for (const Impl::Stream &s : impl_->streams)
        if (s.role != Impl::Stream::Role::Free && s.started && s.track == track) g = std::max(g, s.fade.value);
    return g;
}

float Audio::music_duck() const { return impl_->duck.value * impl_->stinger_duck.value; }

} // namespace lam
