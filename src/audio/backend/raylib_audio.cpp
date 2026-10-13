// raylib audio backend (lam_audio_rl). Slots play sound aliases that are created once and reused,
// so one sound can play on many slots. Streams decode from memory so music works inside an APK.
#include <raylib.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lam/audio/audio.h>
#include <lam/audio/audio_plugin.h>
#include <lam/audio/device.h>
#include <lam/core/app.h>
#include <lam/core/log.h>

namespace lam {

namespace {

constexpr int kSlots = static_cast<int>(Audio::kMaxVoices);

// raylib takes the file type as a NUL-terminated ".ext".
struct ExtBuffer {
    std::array<char, 16> text{};
    explicit ExtBuffer(std::string_view ext) {
        const std::size_t n = ext.size() < text.size() - 1 ? ext.size() : text.size() - 1;
        for (std::size_t i = 0; i < n; ++i) text[i] = ext[i];
    }
    const char *c_str() const { return text.data(); }
};

int to_int_size(std::size_t n) { return n > 0x7FFFFFFFu ? 0 : static_cast<int>(n); }

class RaylibAudioDevice final : public AudioDevice {
public:
    RaylibAudioDevice() = default;
    ~RaylibAudioDevice() override { release_all(); }
    RaylibAudioDevice(const RaylibAudioDevice &) = delete;
    RaylibAudioDevice &operator=(const RaylibAudioDevice &) = delete;

    bool ready() const override { return IsAudioDeviceReady(); }

    std::uint32_t load_sound(std::span<const std::byte> bytes, std::string_view ext) override {
        const int size = to_int_size(bytes.size());
        if (!ready() || size == 0) return 0;
        const ExtBuffer type(ext);
        Wave wave = LoadWaveFromMemory(type.c_str(), reinterpret_cast<const unsigned char *>(bytes.data()), size);
        if (!IsWaveValid(wave)) {
            UnloadWave(wave);
            return 0;
        }
        ::Sound sound = LoadSoundFromWave(wave);
        UnloadWave(wave);
        if (!IsSoundValid(sound)) return 0;
        SoundRec rec;
        rec.source = sound;
        rec.live = true;
        rec.aliases.reserve(static_cast<std::size_t>(kSlots)); // no reallocation while playing
        rec.alias_slot.reserve(static_cast<std::size_t>(kSlots));
        sounds_.push_back(std::move(rec));
        return static_cast<std::uint32_t>(sounds_.size()); // id = index + 1, never reused
    }

    void unload_sound(std::uint32_t id) override {
        SoundRec *rec = find_sound(id);
        if (rec == nullptr) return;
        for (int slot = 0; slot < kSlots; ++slot)
            if (slots_[static_cast<std::size_t>(slot)].sound == id) stop(slot);
        free_sound(*rec);
    }

    void play(std::uint32_t sound, int slot, float volume, float pitch, float pan) override {
        if (slot < 0 || slot >= kSlots) return;
        stop(slot);
        SoundRec *rec = find_sound(sound);
        if (rec == nullptr || !ready()) return;
        // A free alias of this sound, or a new one (warm-up only: capacity is reserved).
        std::size_t alias = rec->aliases.size();
        for (std::size_t i = 0; i < rec->aliases.size(); ++i) {
            if (rec->alias_slot[i] < 0) {
                alias = i;
                break;
            }
        }
        if (alias == rec->aliases.size()) {
            if (rec->aliases.size() >= static_cast<std::size_t>(kSlots)) return;
            ::Sound a = LoadSoundAlias(rec->source);
            if (!IsSoundValid(a)) return;
            rec->aliases.push_back(a);
            rec->alias_slot.push_back(-1);
        }
        rec->alias_slot[alias] = slot;
        Slot &s = slots_[static_cast<std::size_t>(slot)];
        s.sound = sound;
        s.alias = static_cast<int>(alias);
        const ::Sound &a = rec->aliases[alias];
        SetSoundVolume(a, volume);
        SetSoundPitch(a, pitch);
        SetSoundPan(a, pan); // raylib 6.0: -1 left, 1 right
        ::PlaySound(a); // lam::PlaySound is the World event
    }

    void stop(int slot) override {
        ::Sound *a = slot_alias(slot);
        if (a == nullptr) return;
        StopSound(*a);
        Slot &s = slots_[static_cast<std::size_t>(slot)];
        find_sound(s.sound)->alias_slot[static_cast<std::size_t>(s.alias)] = -1;
        s = Slot{};
    }

    void set_slot(int slot, float volume, float pitch, float pan) override {
        ::Sound *a = slot_alias(slot);
        if (a == nullptr) return;
        SetSoundVolume(*a, volume);
        SetSoundPitch(*a, pitch);
        SetSoundPan(*a, pan);
    }

    bool slot_playing(int slot) const override {
        const ::Sound *a = slot_alias(slot);
        return a != nullptr && IsSoundPlaying(*a);
    }

    std::uint32_t open_stream(const std::string &path) override {
        if (!ready() || path.empty()) return 0;
        int size = 0;
        unsigned char *data = LoadFileData(path.c_str(), &size); // raylib file IO (Android-aware)
        if (data == nullptr || size <= 0) {
            if (data != nullptr) UnloadFileData(data);
            return 0;
        }
        const std::uint32_t id = open_music(GetFileExtension(path.c_str()), data, size);
        if (id == 0) {
            UnloadFileData(data);
            return 0;
        }
        streams_[id - 1].owned = data;
        return id;
    }

    std::uint32_t open_stream_memory(std::span<const std::byte> bytes, std::string_view ext) override {
        const int size = to_int_size(bytes.size());
        if (!ready() || size == 0) return 0;
        const ExtBuffer type(ext);
        return open_music(type.c_str(), reinterpret_cast<const unsigned char *>(bytes.data()), size);
    }

    void close_stream(std::uint32_t stream) override {
        StreamRec *s = find_stream(stream);
        if (s == nullptr) return;
        StopMusicStream(s->music);
        UnloadMusicStream(s->music);
        if (s->owned != nullptr) UnloadFileData(s->owned);
        *s = StreamRec{};
    }

    void stream_play(std::uint32_t stream, bool loop) override {
        StreamRec *s = find_stream(stream);
        if (s == nullptr) return;
        s->music.looping = loop; // our own copy of the Music value: no const_cast
        PlayMusicStream(s->music);
        s->playing = true;
        if (paused_) PauseMusicStream(s->music);
    }

    void stream_stop(std::uint32_t stream) override {
        StreamRec *s = find_stream(stream);
        if (s == nullptr) return;
        StopMusicStream(s->music);
        s->playing = false;
    }

    void stream_volume(std::uint32_t stream, float volume) override {
        if (StreamRec *s = find_stream(stream)) SetMusicVolume(s->music, volume);
    }

    void update_streams() override {
        if (paused_) return;
        for (StreamRec &s : streams_) {
            if (!s.live || !s.playing) continue;
            UpdateMusicStream(s.music);
            if (!s.music.looping && !IsMusicStreamPlaying(s.music)) s.playing = false;
        }
    }

    bool stream_playing(std::uint32_t stream) const override {
        const StreamRec *s = find_stream(stream);
        return s != nullptr && s->playing && (paused_ || IsMusicStreamPlaying(s->music));
    }

    void set_paused(bool paused) override {
        if (paused_ == paused) return;
        paused_ = paused;
        for (int slot = 0; slot < kSlots; ++slot) {
            if (::Sound *a = slot_alias(slot)) {
                if (paused) PauseSound(*a);
                else ResumeSound(*a);
            }
        }
        for (StreamRec &s : streams_) {
            if (!s.live || !s.playing) continue;
            if (paused) PauseMusicStream(s.music);
            else ResumeMusicStream(s.music);
        }
    }

private:
    struct SoundRec {
        ::Sound source{};
        std::vector<::Sound> aliases;                           // one per concurrent slot, reused
        std::vector<int> alias_slot;                            // slot playing each alias, -1 free
        bool live = false;
    };
    struct Slot {
        std::uint32_t sound = 0;                                // 0 = idle
        int alias = -1;
    };
    struct StreamRec {
        ::Music music{};
        unsigned char *owned = nullptr;                         // LoadFileData buffer (open_stream)
        bool live = false;
        bool playing = false;
    };

    SoundRec *find_sound(std::uint32_t id) {
        if (id == 0 || id > sounds_.size()) return nullptr;
        SoundRec &r = sounds_[id - 1];
        return r.live ? &r : nullptr;
    }
    const SoundRec *find_sound(std::uint32_t id) const {
        if (id == 0 || id > sounds_.size()) return nullptr;
        const SoundRec &r = sounds_[id - 1];
        return r.live ? &r : nullptr;
    }
    StreamRec *find_stream(std::uint32_t id) {
        if (id == 0 || id > streams_.size()) return nullptr;
        StreamRec &r = streams_[id - 1];
        return r.live ? &r : nullptr;
    }
    const StreamRec *find_stream(std::uint32_t id) const {
        if (id == 0 || id > streams_.size()) return nullptr;
        const StreamRec &r = streams_[id - 1];
        return r.live ? &r : nullptr;
    }
    ::Sound *slot_alias(int slot) {
        if (slot < 0 || slot >= kSlots) return nullptr;
        const Slot &s = slots_[static_cast<std::size_t>(slot)];
        SoundRec *rec = find_sound(s.sound);
        return rec != nullptr ? &rec->aliases[static_cast<std::size_t>(s.alias)] : nullptr;
    }
    const ::Sound *slot_alias(int slot) const {
        if (slot < 0 || slot >= kSlots) return nullptr;
        const Slot &s = slots_[static_cast<std::size_t>(slot)];
        const SoundRec *rec = find_sound(s.sound);
        return rec != nullptr ? &rec->aliases[static_cast<std::size_t>(s.alias)] : nullptr;
    }

    std::uint32_t open_music(const char *type, const unsigned char *data, int size) {
        ::Music music = LoadMusicStreamFromMemory(type, data, size);
        if (!IsMusicValid(music)) return 0;
        // Reuse a closed record (ids stay small); a stream id is never live twice at once.
        for (std::size_t i = 0; i < streams_.size(); ++i) {
            if (!streams_[i].live) {
                streams_[i] = StreamRec{music, nullptr, true, false};
                return static_cast<std::uint32_t>(i + 1);
            }
        }
        streams_.push_back(StreamRec{music, nullptr, true, false});
        return static_cast<std::uint32_t>(streams_.size());
    }

    void free_sound(SoundRec &rec) {
        for (::Sound &a : rec.aliases) {
            StopSound(a);
            UnloadSoundAlias(a);
        }
        rec.aliases.clear();
        rec.alias_slot.clear();
        UnloadSound(rec.source);
        rec.source = ::Sound{};
        rec.live = false;
    }

    // Unloads everything this device created; the platform closes the device afterwards.
    void release_all() {
        if (!IsAudioDeviceReady()) return; // the device is gone; raylib freed its buffers already
        for (int slot = 0; slot < kSlots; ++slot) stop(slot);
        for (std::size_t i = 0; i < streams_.size(); ++i)
            if (streams_[i].live) close_stream(static_cast<std::uint32_t>(i + 1));
        for (SoundRec &rec : sounds_)
            if (rec.live) free_sound(rec);
    }

    std::vector<SoundRec> sounds_;                              // id = index + 1
    std::array<Slot, static_cast<std::size_t>(kSlots)> slots_{};
    std::vector<StreamRec> streams_;                            // id = index + 1
    bool paused_ = false;
};

} // namespace

void RaylibAudioPlugin::build(App &app) {
    app.require_plugin("lam.audio", name());
    Audio *audio = app.try_res<Audio>();
    if (audio == nullptr) {
        LAM_LOG_ERROR("RaylibAudioPlugin: add AudioPlugin first; sound stays off");
        return;
    }
    if (!app.config().audio) {
        LAM_LOG_INFO("audio: AppConfig::audio is off; running silent");
        return;
    }
    if (!IsAudioDeviceReady()) LAM_LOG_WARN("audio: the audio device is not open; running silent");
    audio->set_device(std::make_unique<RaylibAudioDevice>());
}

void RaylibAudioPlugin::shutdown(App &app) {
    // Runs before PlatformPlugin's shutdown, so sounds are unloaded while the device is open.
    if (Audio *audio = app.try_res<Audio>()) audio->set_device(nullptr);
}

} // namespace lam
