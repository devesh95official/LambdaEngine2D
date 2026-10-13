#pragma once
// Input drivers, recording and replay. Replay files (little-endian): "LAMR", u16 version, u16
// tick_rate, u64 seed, u8 players, build_id and os (u16 length + bytes), then per tick a u64
// app_tick and the raw InputFrames.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <lam/core/app.h>
#include <lam/core/result.h>
#include <lam/input/input_frame.h>
#include <lam/input/raw_input.h>
namespace lam {
namespace detail {
struct InputAccess;
}
// Edits or replaces each tick's input: bots, autoplay, replays, scripted tests.
class InputDriver {
public:
    virtual ~InputDriver() = default;
    // ctx.world() is the focused world. During a scene transition nothing is focused and the
    // frames go nowhere, so drivers driving menus should repeat an intent until it's taken.
    // Must not allocate in steady state.
    virtual void fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) = 0;
};
struct ReplayHeader {
    std::uint16_t version = 1;
    std::uint16_t tick_rate = 60;
    std::uint64_t seed = 0;
    std::uint8_t players = 1;                                   // frames per tick
    std::string build_id;
    std::string os;
};
inline constexpr std::uint16_t kReplayVersion = 1;
// Fills build_id and os for this build.
ReplayHeader current_replay_header(std::uint64_t seed, int tick_rate, int players);
// Version and tick rate must match; build and OS too unless `any_build` (--replay-any-build).
Status replay_compatible(const ReplayHeader &h, int tick_rate, bool any_build);
class InputRecorder {                                           // --record-input
public:
    InputRecorder();
    ~InputRecorder();
    InputRecorder(const InputRecorder &) = delete;
    InputRecorder &operator=(const InputRecorder &) = delete;
    // Truncates `path` and writes the header. The write buffer is the only allocation.
    Status open(const std::string &path, const ReplayHeader &header);
    // Ticks must increase. Does nothing when not open.
    void record(std::uint64_t app_tick, std::span<const InputFrame, kMaxPlayers> frames);
    // Reports an Io error if any write failed since open().
    Status close();
    bool is_open() const;
    std::uint64_t ticks_recorded() const;
    const ReplayHeader &header() const;
private:
    Status flush();
    std::FILE *file_ = nullptr;
    std::vector<std::byte> buffer_;
    ReplayHeader header_;
    std::string path_;
    std::uint64_t ticks_ = 0;
    std::uint64_t last_tick_ = 0;
    bool failed_ = false;
};
class ReplayDriver final : public InputDriver {                 // --replay
public:
    // Loads the whole file. A truncated last record is dropped with a warning.
    static Result<std::unique_ptr<ReplayDriver>> open(const std::string &path);
    const ReplayHeader &header() const;
    bool finished() const;
    std::size_t tick_count() const;
    std::uint64_t first_tick() const;                           // 0 if empty
    std::uint64_t last_tick() const;
    // Ticks without a record get neutral frames, so live input never leaks into a replay.
    void fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) override;
private:
    ReplayDriver() = default;
    ReplayHeader header_;
    std::vector<std::byte> data_;                               // records only
    std::size_t record_size_ = 0;                               // 8 + players * sizeof(InputFrame)
    std::size_t count_ = 0;
    std::size_t cursor_ = 0;
};
class InputSystem {                                             // App resource
public:
    InputSystem();
    ~InputSystem();
    InputSystem(const InputSystem &) = delete;
    InputSystem &operator=(const InputSystem &) = delete;
    void set_driver(InputDriver *driver);                       // not owned; null = live devices
    InputDriver *driver() const;
    // Not owned. Clear it before destroying the recorder.
    void set_recorder(InputRecorder *recorder);
    InputRecorder *recorder() const;
    // Called by the backend each frame (or by tests). Wheel, text and touch-began only count for
    // the next frame.
    void feed_raw(const RawInput &r);
    const RawInput &raw() const;
    // UTF-32, valid until the next frame update.
    std::span<const char32_t> text_typed() const;
    // From frame code. Reaches the next tick exactly once; past kMaxIntents they're dropped.
    void queue_intent(const Intent &intent, int player = 0);
    std::uint32_t dropped_intents() const;
    // Drops undelivered presses and wheel motion; keeps intents and held state. Called on pause,
    // focus changes and FocusLost/Background.
    void reset_latch();
private:
    friend struct detail::InputAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
