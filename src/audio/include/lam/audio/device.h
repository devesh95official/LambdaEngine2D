#pragma once
// Audio device interface, called only by Audio on the main thread. Sounds play on voice slots
// (0 .. kMaxVoices-1). Volume is linear, pitch is a rate, pan is [-1, 1].
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace lam {
class AudioDevice {
public:
    virtual ~AudioDevice() = default;
    // False without audio hardware; Audio still runs, silently.
    virtual bool ready() const = 0;
    // `ext` is lower case with the dot (".wav"). Returns 0 on failure.
    virtual std::uint32_t load_sound(std::span<const std::byte> bytes, std::string_view ext) = 0;
    // Also stops every slot playing it.
    virtual void unload_sound(std::uint32_t id) = 0;
    // Replaces whatever the slot was playing. No allocation after warm-up.
    virtual void play(std::uint32_t sound, int slot, float volume, float pitch, float pan) = 0;
    virtual void stop(int slot) = 0;
    virtual void set_slot(int slot, float volume, float pitch, float pan) = 0;
    // False once a one-shot ended, after stop() and while paused.
    virtual bool slot_playing(int slot) const = 0;
    // `path` is absolute. Returns 0 on failure.
    virtual std::uint32_t open_stream(const std::string &path) = 0;
    virtual void close_stream(std::uint32_t stream) = 0;
    // Starts from the beginning.
    virtual void stream_play(std::uint32_t stream, bool loop) = 0;
    // Also rewinds.
    virtual void stream_stop(std::uint32_t stream) = 0;
    virtual void stream_volume(std::uint32_t stream, float volume) = 0;
    // Called once per frame by Audio::update.
    virtual void update_streams() = 0;

    // The rest have defaults so older devices still compile.
    // Streams from bytes that must stay alive until close_stream. 0 makes Audio fall back to
    // open_stream(path).
    virtual std::uint32_t open_stream_memory(std::span<const std::byte> bytes, std::string_view ext) {
        (void)bytes;
        (void)ext;
        return 0;
    }
    // Devices that can't tell return true; Audio then times stingers by MusicTrack::seconds.
    virtual bool stream_playing(std::uint32_t stream) const {
        (void)stream;
        return true;
    }
    // Pause or resume every slot and stream in place (app in the background).
    virtual void set_paused(bool paused) { (void)paused; }
};
// Records calls for tests and headless runs and fakes slot and stream state. The first call
// reserves the whole record buffer so later recording doesn't allocate.
class NullAudioDevice final : public AudioDevice {
public:
    // StreamPlay stores loop in `slot`, StreamVolume the volume in `volume`.
    struct Call {
        enum class Kind : std::uint8_t { LoadSound, UnloadSound, Play, Stop, SetSlot, OpenStream, CloseStream,
            StreamPlay, StreamStop, StreamVolume };
        Kind kind = Kind::Play;
        std::uint32_t id = 0;
        int slot = -1;
        float volume = 0, pitch = 0, pan = 0;
    };
    // Recording stops after this many calls until clear_calls().
    static constexpr std::size_t kMaxRecordedCalls = 16384;
    bool ready() const override;
    std::uint32_t load_sound(std::span<const std::byte> bytes, std::string_view ext) override;
    void unload_sound(std::uint32_t id) override;
    void play(std::uint32_t sound, int slot, float volume, float pitch, float pan) override;
    void stop(int slot) override;
    void set_slot(int slot, float volume, float pitch, float pan) override;
    bool slot_playing(int slot) const override;
    std::uint32_t open_stream(const std::string &path) override;
    void close_stream(std::uint32_t stream) override;
    void stream_play(std::uint32_t stream, bool loop) override;
    void stream_stop(std::uint32_t stream) override;
    void stream_volume(std::uint32_t stream, float volume) override;
    void update_streams() override;
    std::uint32_t open_stream_memory(std::span<const std::byte> bytes, std::string_view ext) override;
    bool stream_playing(std::uint32_t stream) const override;
    void set_paused(bool paused) override;
    // Valid until the next device call or clear_calls().
    std::span<const Call> calls() const;
    void clear_calls();                                         // keeps the capacity
    std::size_t count(Call::Kind kind) const;
    // Fake the end of a one-shot or a non-looping stream.
    void finish(int slot);
    void finish_stream(std::uint32_t stream);
    bool paused() const;
private:
    void record(const Call &c);
    std::vector<Call> calls_;
    std::array<bool, 64> playing_{};
    std::uint32_t next_id_ = 1;
    std::vector<std::uint32_t> playing_streams_;                // ids between stream_play and stop
    bool paused_ = false;
};
}
