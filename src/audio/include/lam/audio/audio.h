#pragma once
// Audio service: voices, buses, sound banks, music, stingers and ambience.
// Bus volumes persist in SettingsStore as "audio.<bus>.volume".
//   audio.add_bank(assets.load<SoundBank>("sounds/sfx.sounds.json"));      // startup
//   VoiceHandle v = audio.play("coin"_id);       // frame code; tick systems emit PlaySound instead
//   audio.play_music(assets.load<MusicTrack>("music/theme.ogg"));          // crossfades
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <lam/assets/asset_server.h>
#include <lam/audio/components.h>
#include <lam/audio/device.h>
#include <lam/core/asset_id.h>
#include <lam/core/id.h>
#include <lam/math/vec2.h>
namespace lam {
class SettingsStore;
namespace detail {
struct AudioAccess;                                             // loaders and systems
}
// A short sound uploaded to the device once. Without a device it is Ready and silent.
struct SoundClip {                                              // asset (.wav .ogg .mp3 .qoa .flac)
    std::uint32_t device_id = 0;                                // set at finalize; 0 = silent
    float seconds = 0;                                          // length; 0 if unknown (.mp3)
    std::vector<std::byte> encoded;                             // decode -> finalize; freed after upload
    std::string extension;                                      // ".wav" (lower case, with the dot)
    std::uint32_t device_epoch = 0;                             // Audio device generation of device_id
};
// A streamed track (music, stinger or ambience). The device decodes from the in-memory bytes,
// so nothing is opened at play time (APK-safe).
struct MusicTrack {                                             // asset; absolute path, streamed
    std::string path;                                           // absolute (AssetServer::resolve_path)
    float seconds = 0;                                          // length; 0 if unknown (.mp3/.xm/.mod)
    std::vector<std::byte> encoded;                             // the whole file (streamed from memory)
    std::string extension;                                      // ".ogg" (lower case, with the dot)
};
// Named sound groups from a ".sounds.json" file (format in src/audio/README.md).
// Ready only once every variant is.
struct SoundBank {                                              // asset ".sounds.json"
    struct Group {
        StrId id;                                               // play(StrId) key
        std::vector<AssetId<SoundClip>> variants;               // one is picked per play, never twice in a row
        float volume = 1;                                       // [0, 2], times PlayParams::volume
        float pitch_jitter = 0.04f;                             // pitch *= 1 + uniform(-j, j); [0, 0.5]
        StrId bus;                                              // bus name; none = "sfx"
        std::uint8_t max_voices = 3;                            // [1, 64]; the oldest is stolen beyond
        std::uint16_t min_interval_ms = 30;                     // plays sooner than this (real time) are dropped
    };
    std::vector<Group> groups;
};
// Bus index (Audio::add_bus). Parents always have smaller ids than their children.
using BusId = std::uint8_t;                                     // 0 = master
// Per-play settings; every field has a default.
struct PlayParams {
    float volume = 1;                                           // linear gain, times group and bus volumes
    float pitch = 1;                                            // playback rate (> 0); jitter multiplies it
    float pan = 0;                                              // [-1 left, 1 right]; added to the spatial pan
    bool loop = false;                                          // restarted when it ends, until stop()
    BusId bus = 0;                                              // 0 = default: the group's bus, else "sfx"
    Vec2 position;                                              // world pixels (spatial only)
    bool spatial = false;                                       // relative to the listener (set_listener)
    float range = 0;                                            // spatial: silent beyond (px); 0 = the listener's range
};
// Gain and pan of a sound at `source` heard at `listener`.
struct SpatialMix { float gain = 1; float pan = 0; };
// Full gain within range/4, then (1 - t)^2 down to 0 at `range`; pan = dx / range in [-0.8, 0.8].
// Out of range, range <= 0 or non-finite input give {0, 0}.
SpatialMix spatial_mix(Vec2 listener, Vec2 source, float range);
// Length in seconds read from the header of a .wav, .ogg, .qoa or .flac file; 0 if unknown.
float encoded_sound_seconds(std::span<const std::byte> bytes, std::string_view ext);
// App resource, main thread only. Unknown ids and stale handles are ignored. play(), update()
// and the voice setters don't allocate. Voice stealing rules are in src/audio/README.md.
class Audio {
public:
    static constexpr std::uint32_t kMaxVoices = 64;             // device slots
    static constexpr std::uint32_t kMaxBuses = 32;              // including master
    static constexpr std::uint32_t kMaxStreams = 16;            // music + stingers + ambience layers
    // Creates master, sfx, ui, music and ambience (ids 0-4) on a NullAudioDevice.
    explicit Audio(AssetServer &assets);
    ~Audio();
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    // Null installs a NullAudioDevice. Streams reopen on the new device; clips loaded on the old
    // one stay silent until reloaded, so install the backend before loading sounds.
    void set_device(std::unique_ptr<AudioDevice> device);
    // Valid until set_device.
    AudioDevice &device();
    // Returns the existing bus of that name if there is one. Past kMaxBuses: asserts, returns master.
    BusId add_bus(std::string_view name, BusId parent = 0);
    // Master (with a warning) if unknown. Look it up once, not per play.
    BusId bus(std::string_view name) const;                     // "master", "sfx", "ui", "music", "ambience"
    // Clamped to [0, 1] and written to the attached settings.
    void set_bus_volume(BusId b, float volume);
    // The bus's own volume; 0 for an unknown bus.
    float bus_volume(BusId b) const;
    // Voices and streams keep running at volume 0. Not persisted.
    void set_muted(bool muted);
    bool muted() const;
    // Set by audio_world from the focused world's AudioListener. range is the default for spatial plays.
    void set_listener(Vec2 position, float range);
    // Null handle if the clip isn't Ready, was throttled, is out of range, audio is suspended
    // or no voice could be freed.
    VoiceHandle play(AssetId<SoundClip> clip, const PlayParams &p = {});
    // Plays a variant of a bank group (never the previous one) with the group's jitter, volume,
    // bus and limits. Null handle as above, or for an unknown group.
    VoiceHandle play(StrId group, const PlayParams &p = {});
    // fade_seconds <= 0 stops at once, otherwise fades out linearly.
    void stop(VoiceHandle v, float fade_seconds = 0);
    // False once the voice ended, was stopped or was stolen.
    bool playing(VoiceHandle v) const;
    void set_volume(VoiceHandle v, float volume);               // linear gain >= 0
    void set_pitch(VoiceHandle v, float pitch);                 // jitter still applies; clamped to [0.1, 4]
    void set_pan(VoiceHandle v, float pan);                     // [-1, 1]; added to the spatial pan
    // Groups become playable once the bank is Ready and follow hot reloads. Idempotent.
    void add_bank(AssetId<SoundBank> bank);
    // Crossfades to a looping track. The current track again does nothing; null fades out.
    void play_music(AssetId<MusicTrack> track, float crossfade = 1.5f);
    // Plays once over the music, ducking it to `duck` until the stinger ends. Replaces the last one.
    void play_stinger(AssetId<MusicTrack> track, float duck = 0.3f);
    // Pause menus use 0.3; 1 brings the music back.
    void set_music_duck(float gain, float fade = 0.4f);
    // Each loop is its own layer. gain 0 fades the layer out and closes it.
    void set_ambience(AssetId<MusicTrack> loop, float gain, float fade = 1.5f);
    // Called once per frame (Update +1000). Bad or negative dt counts as 0.
    void update(float dt);
    // Plays that passed the throttle, including silent ones.
    std::uint32_t plays() const;
    // Voices currently held (playing or fading out).
    std::uint32_t active_voices() const;

    // RNG for variants and jitter. AudioPlugin seeds it from the App seed.
    void set_seed(std::uint64_t seed);
    // Syncs bus volumes and "audio.mute_unfocused" with `settings`; outside changes apply on the
    // next update(). Null detaches.
    void attach_settings(SettingsStore *settings);
    std::uint32_t bus_count() const;
    std::string_view bus_name(BusId b) const;                   // "" if unknown
    BusId bus_parent(BusId b) const;                            // master is its own parent
    // Own volume times every ancestor's. Mute is not included.
    float bus_gain(BusId b) const;
    // Background mode: pauses the device, freezes voices and streams, drops new plays.
    void set_suspended(bool suspended);
    bool suspended() const;
    // For moving emitters; ignored by non-spatial voices.
    void set_position(VoiceHandle v, Vec2 position);
    // [1, kMaxVoices]. Voices above a lowered cap play out.
    void set_voice_limit(std::uint32_t n);
    std::uint32_t voice_limit() const;
    // Throttle for play(AssetId), default 0.03 s, 0 disables. Groups use min_interval_ms.
    void set_clip_min_interval(float seconds);
    bool has_group(StrId group) const;
    // Stops every voice and the stinger. Music and ambience keep playing.
    void stop_all(float fade_seconds = 0);
    void silence_ambience(float fade = 1.0f);
    // Last track asked for by play_music(); null after a fade out.
    AssetId<MusicTrack> music() const;
    // Fade gain of the track's stream (largest if it plays in several roles), without bus volumes.
    float track_gain(AssetId<MusicTrack> track) const;
    // set_music_duck gain times the stinger duck.
    float music_duck() const;
    // Seconds advanced by update() so far.
    double real_time() const;
private:
    friend struct detail::AudioAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
