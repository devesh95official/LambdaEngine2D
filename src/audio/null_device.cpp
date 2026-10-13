// NullAudioDevice: records calls and fakes slot/stream state so Audio behaves as with a real device.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <lam/audio/device.h>
#include <lam/core/log.h>

namespace lam {

namespace {
bool valid_slot(int slot) { return slot >= 0 && slot < 64; }
// Streams playing at once that fit without growing (Audio plays at most Audio::kMaxStreams).
constexpr std::size_t kStreamReserve = 32;
} // namespace

void NullAudioDevice::record(const Call &c) {
    // Reserve once for every call plus the overflow sentinel.
    if (calls_.capacity() == 0) calls_.reserve(kMaxRecordedCalls + 1);
    if (calls_.size() >= kMaxRecordedCalls) {
        if (calls_.size() == kMaxRecordedCalls) {
            LAM_LOG_WARN("NullAudioDevice: {} calls recorded; recording stops until clear_calls()",
                         kMaxRecordedCalls);
            calls_.push_back(c); // one sentinel past the limit marks "warned"
        }
        return;
    }
    calls_.push_back(c);
}

bool NullAudioDevice::ready() const { return true; }

std::uint32_t NullAudioDevice::load_sound(std::span<const std::byte> bytes, std::string_view ext) {
    (void)ext;
    const std::uint32_t id = bytes.empty() ? 0 : next_id_++;
    record({Call::Kind::LoadSound, id, -1, 0, 0, 0});
    return id;
}

void NullAudioDevice::unload_sound(std::uint32_t id) { record({Call::Kind::UnloadSound, id, -1, 0, 0, 0}); }

void NullAudioDevice::play(std::uint32_t sound, int slot, float volume, float pitch, float pan) {
    record({Call::Kind::Play, sound, slot, volume, pitch, pan});
    if (valid_slot(slot)) playing_[static_cast<std::size_t>(slot)] = true;
}

void NullAudioDevice::stop(int slot) {
    record({Call::Kind::Stop, 0, slot, 0, 0, 0});
    if (valid_slot(slot)) playing_[static_cast<std::size_t>(slot)] = false;
}

void NullAudioDevice::set_slot(int slot, float volume, float pitch, float pan) {
    record({Call::Kind::SetSlot, 0, slot, volume, pitch, pan});
}

bool NullAudioDevice::slot_playing(int slot) const {
    return valid_slot(slot) && playing_[static_cast<std::size_t>(slot)];
}

std::uint32_t NullAudioDevice::open_stream(const std::string &path) {
    const std::uint32_t id = path.empty() ? 0 : next_id_++;
    record({Call::Kind::OpenStream, id, -1, 0, 0, 0});
    return id;
}

std::uint32_t NullAudioDevice::open_stream_memory(std::span<const std::byte> bytes, std::string_view ext) {
    (void)ext;
    const std::uint32_t id = bytes.empty() ? 0 : next_id_++;
    record({Call::Kind::OpenStream, id, -1, 0, 0, 0});
    return id;
}

void NullAudioDevice::close_stream(std::uint32_t stream) {
    record({Call::Kind::CloseStream, stream, -1, 0, 0, 0});
    std::erase(playing_streams_, stream);
}

void NullAudioDevice::stream_play(std::uint32_t stream, bool loop) {
    record({Call::Kind::StreamPlay, stream, loop ? 1 : 0, 0, 0, 0});
    if (stream == 0 || std::find(playing_streams_.begin(), playing_streams_.end(), stream) != playing_streams_.end())
        return;
    if (playing_streams_.capacity() == 0) playing_streams_.reserve(kStreamReserve);
    playing_streams_.push_back(stream);
}

void NullAudioDevice::stream_stop(std::uint32_t stream) {
    record({Call::Kind::StreamStop, stream, -1, 0, 0, 0});
    std::erase(playing_streams_, stream);
}

void NullAudioDevice::stream_volume(std::uint32_t stream, float volume) {
    record({Call::Kind::StreamVolume, stream, -1, volume, 0, 0});
}

void NullAudioDevice::update_streams() {}

bool NullAudioDevice::stream_playing(std::uint32_t stream) const {
    return std::find(playing_streams_.begin(), playing_streams_.end(), stream) != playing_streams_.end();
}

void NullAudioDevice::set_paused(bool paused) { paused_ = paused; }

std::span<const NullAudioDevice::Call> NullAudioDevice::calls() const {
    return std::span<const Call>(calls_.data(), std::min(calls_.size(), kMaxRecordedCalls));
}

void NullAudioDevice::clear_calls() { calls_.clear(); }

std::size_t NullAudioDevice::count(Call::Kind kind) const {
    std::size_t n = 0;
    for (const Call &c : calls())
        if (c.kind == kind) ++n;
    return n;
}

void NullAudioDevice::finish(int slot) {
    if (valid_slot(slot)) playing_[static_cast<std::size_t>(slot)] = false;
}

void NullAudioDevice::finish_stream(std::uint32_t stream) { std::erase(playing_streams_, stream); }

bool NullAudioDevice::paused() const { return paused_; }

} // namespace lam
