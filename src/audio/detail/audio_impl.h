#pragma once
// Private state of lam::Audio. Fixed arrays apart from bus names, banks and the group index,
// which only change on cold paths.
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <lam/audio/audio.h>
#include <lam/core/result.h>
#include <lam/ecs/entity.h>
#include <lam/math/rng.h>
#include <lam/math/scalar.h>

namespace lam {
class World;
namespace detail {

// Linear ramp that reaches its target exactly `seconds` after to() (fades, crossfades, ducks).
struct Ramp {
    float value = 0;
    float target = 0;
    float rate = 0;                                             // units per second; 0 = settled
    void snap(float v) {
        value = target = v;
        rate = 0;
    }
    void to(float t, float seconds) {
        target = t;
        const float d = std::fabs(t - value);
        if (!(seconds > 0.0f) || !std::isfinite(seconds) || d == 0.0f) {
            snap(t);
            return;
        }
        rate = d / seconds;
    }
    void update(float dt) {
        if (rate <= 0.0f || !(dt > 0.0f)) return;
        value = approach(value, target, rate * dt);
        if (value == target) rate = 0;
    }
};

// Seconds of a stinger's attack and release of the music duck, and of a replaced stinger's fade.
inline constexpr float kStingerAttack = 0.25f;
inline constexpr float kStingerRelease = 1.0f;
inline constexpr float kStingerReplaceFade = 0.2f;
// A non-looping stream whose device cannot report its end stops this long after its length.
inline constexpr float kStreamEndMargin = 0.1f;
// Pitch sent to the device is clamped to this range.
inline constexpr float kMinPitch = 0.1f;
inline constexpr float kMaxPitch = 4.0f;
// Default bus ids (created by the Audio constructor in this order).
inline constexpr BusId kBusMaster = 0, kBusSfx = 1, kBusUi = 2, kBusMusic = 3, kBusAmbience = 4;

} // namespace detail

struct Audio::Impl {
    struct Bus {
        std::string name;
        StrId id;
        BusId parent = 0;
        float volume = 1;
        std::string key;                                        // "audio.<name>.volume"
    };
    struct Voice {
        std::uint32_t generation = 1;                           // of the current (or next) voice here
        bool active = false;
        bool retired = false;                                   // generation exhausted
        bool loop = false;
        bool spatial = false;
        bool stopping = false;                                  // fading out
        BusId bus = 0;
        std::uint32_t sound = 0;                                // device sound id
        StrId group;                                            // bank group; none for clip plays
        float base = 1;                                         // group volume
        float volume = 1;                                       // PlayParams / set_volume
        float pitch = 1;                                        // PlayParams / set_pitch
        float jitter = 1;                                       // pitch factor picked at play
        float pan = 0;
        float range = 0;                                        // spatial; 0 = the listener's
        Vec2 position;
        float fade = 1;                                         // fade-out gain
        float fade_rate = 0;                                    // per second while stopping
        std::uint64_t order = 0;                                // play counter: lower = older
        float out_volume = -1, out_pitch = -1, out_pan = -9;   // last values sent to the device
        std::uint32_t owner_world = 0;                          // looping emitter: World::id()
        Entity owner;                                           // looping emitter entity
    };
    struct Group {                                              // index entry of a Ready bank's group
        StrId id;
        AssetId<SoundBank> bank;
        std::uint32_t index = 0;                                // into SoundBank::groups
        BusId bus = detail::kBusSfx;
        int last_variant = -1;
        double last_play = -1e30;
    };
    struct BankRef {
        AssetId<SoundBank> id;
        bool ready = false;
        bool failed_warned = false;
        std::uint32_t version = 0;
    };
    struct Stream {
        enum class Role : std::uint8_t { Free, Music, Stinger, Ambience };
        Role role = Role::Free;
        AssetId<MusicTrack> track;
        bool loop = true;
        bool started = false;                                   // opened (or found silent) and playing
        std::uint32_t device_stream = 0;                        // 0 = silent (no device/failed)
        const MusicTrack *source = nullptr;                     // the asset value the stream reads
        detail::Ramp fade;
        float elapsed = 0;                                      // seconds since started
        float out_volume = -1;
        float duck = 1;                                         // stinger: music duck while it plays
    };
    struct RecentClip {
        std::uint64_t clip = 0;                                 // AssetId bits
        double time = -1e30;
    };
    enum class StartResult : std::uint8_t { Waiting, Started, Failed };

    explicit Impl(AssetServer &a);

    // ---- shared helpers (audio.cpp) ----
    float gate() const { return (muted || focus_muted || suspended) ? 0.0f : 1.0f; }
    void recompute_buses();
    float chain_gain(BusId b) const;
    void define_bus_setting(Bus &b);
    void sync_settings();
    Voice *live(VoiceHandle h);
    const Voice *live(VoiceHandle h) const;
    void voice_output(const Voice &v, float &volume, float &pitch, float &pan) const;
    void apply(std::uint32_t slot, bool force);
    void apply_all();
    void free_voice(std::uint32_t slot);
    void kill(std::uint32_t slot);
    int acquire(StrId group, std::uint32_t max_voices);
    VoiceHandle start(int slot, std::uint32_t sound, const PlayParams &p, StrId group, BusId bus,
                      float base, float jitter);
    bool spatial_silent(const PlayParams &p) const;
    bool clip_throttled(std::uint64_t clip) const;
    void note_clip(std::uint64_t clip);
    Group *find_group(StrId id);
    const Group *find_group(StrId id) const;
    void refresh_banks();
    bool banks_loading() const;                                 // an added bank is Queued or Loading
    void rebuild_groups();
    BusId resolve_bus(StrId name);
    bool warn_once(std::uint64_t key);
    void update_voices(float dt);
    void shutdown();
    // ---- streams (audio_streams.cpp) ----
    int find_stream(Stream::Role role, AssetId<MusicTrack> track) const;
    int alloc_stream(Stream::Role role, AssetId<MusicTrack> track, bool loop);
    void close_stream(Stream &s);
    void free_stream(int i);
    float stream_gain(const Stream &s) const;
    void apply_stream(Stream &s, bool force);
    StartResult start_stream(int i);
    void try_start(int i);
    void update_streams(float dt);
    void release_track(const MusicTrack *track);
    void detach_streams();

    AssetServer &assets;
    std::unique_ptr<AudioDevice> device;
    std::uint32_t device_epoch = 1;
    // buses
    std::vector<Bus> buses;
    std::array<float, kMaxBuses> bus_gain{};
    SettingsStore *settings = nullptr;
    std::uint32_t settings_revision = 0;
    bool mute_unfocused = false;
    // gates
    bool muted = false;
    bool focus_lost = false;
    bool focus_muted = false;
    bool suspended = false;
    // listener
    Vec2 listener;
    float listener_range = 240;
    // voices
    std::array<Voice, kMaxVoices> voices{};
    std::uint32_t voice_limit = kMaxVoices;
    std::uint64_t play_counter = 0;
    std::uint32_t plays = 0;
    std::uint32_t active = 0;
    // time and throttles
    double now = 0;
    float clip_interval = 0.03f;
    std::array<RecentClip, 64> recent{};
    std::uint32_t recent_next = 0;
    // banks
    std::vector<BankRef> banks;
    std::vector<Group> groups;                                  // sorted by id.value
    // presentation RNG (variants, jitter); never hashed
    Rng rng{0xA0D10};
    // de-duplicated warnings
    std::array<std::uint64_t, 32> warned{};
    std::uint32_t warned_count = 0;
    bool warned_overflow = false;                               // the table filled up (logged once)
    // streams
    std::array<Stream, kMaxStreams> streams{};
    AssetId<MusicTrack> music;
    int stinger = -1;
    detail::Ramp duck;
    detail::Ramp stinger_duck;
};

namespace detail {
// Access to Audio's internals for the module's own loaders and systems.
struct AudioAccess {
    static Audio::Impl &impl(Audio &a) { return *a.impl_; }
    static const Audio::Impl &impl(const Audio &a) { return *a.impl_; }
    // SoundClip finalize: uploads `clip.encoded` to the device and frees it (silent if no device).
    static Status upload_clip(Audio &a, SoundClip &clip);
    // SoundClip unload: stops its voices and unloads it from the device it was uploaded to.
    static void release_clip(Audio &a, SoundClip &clip);
    // MusicTrack unload: closes every stream reading `track` (they reopen if it reloads).
    static void release_track(Audio &a, const MusicTrack &track);
    // Marks a looping emitter voice as owned by (world, entity) for the orphan sweep.
    static void set_owner(Audio &a, VoiceHandle v, std::uint32_t world, Entity e);
    // The group a live voice plays (none for clips and stale handles).
    static StrId voice_group(const Audio &a, VoiceHandle v);
    // Group at max_voices, not counting fading voices. Looping emitters wait on this instead
    // of stealing each other's voices.
    static bool group_full(Audio &a, StrId group);
    // Stops every voice owned by `world_id` (shutdown_world).
    static void stop_world(Audio &a, std::uint32_t world_id);
    // Stops owned voices whose entity is gone, lost its AudioEmitter, or holds another handle.
    static void sweep_world(Audio &a, World &w);
    // FocusLost/FocusGained: mutes while unfocused if "audio.mute_unfocused" is set.
    static void set_focus_lost(Audio &a, bool lost);
    // Plugin shutdown: stops every voice and closes every stream.
    static void shutdown(Audio &a);
};
} // namespace detail
} // namespace lam
