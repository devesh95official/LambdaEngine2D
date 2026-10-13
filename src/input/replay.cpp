// Replay files. Frames are stored as raw bytes, so a file only works with the build that wrote
// it; the header has the build id, OS and tick rate to check that.
#include <lam/input/driver.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <format>
#include <string_view>

#include <lam/core/build_info.h>
#include <lam/core/fs.h>
#include <lam/core/log.h>

#include "detail/input_state.h"

namespace lam {
namespace {

static_assert(std::endian::native == std::endian::little, "replay files assume a little-endian host");

constexpr char kMagic[4] = {'L', 'A', 'M', 'R'};
constexpr std::size_t kFlushBytes = 64 * 1024;

void put_u16(std::vector<std::byte> &out, std::uint16_t v) {
    out.push_back(static_cast<std::byte>(v & 0xFFu));
    out.push_back(static_cast<std::byte>(v >> 8));
}
void put_u64(std::byte *out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}
void put_u64(std::vector<std::byte> &out, std::uint64_t v) {
    std::byte b[8];
    put_u64(b, v);
    out.insert(out.end(), b, b + 8);
}
void put_str(std::vector<std::byte> &out, std::string_view s) {
    put_u16(out, static_cast<std::uint16_t>(s.size()));
    for (char c : s)
        out.push_back(static_cast<std::byte>(c));
}

// Bounds-checked little-endian reader.
struct Reader {
    std::span<const std::byte> data;
    std::size_t pos = 0;
    bool ok = true;
    bool need(std::size_t n) {
        if (!ok || data.size() - pos < n) ok = false;
        return ok;
    }
    std::uint64_t uint(std::size_t bytes) {
        if (!need(bytes)) return 0;
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < bytes; ++i)
            v |= static_cast<std::uint64_t>(data[pos + i]) << (8 * i);
        pos += bytes;
        return v;
    }
    std::string str() {
        const auto n = static_cast<std::size_t>(uint(2));
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char *>(data.data() + pos), n);
        pos += n;
        return s;
    }
};

std::uint64_t read_u64(const std::byte *p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

} // namespace

ReplayHeader current_replay_header(std::uint64_t seed, int tick_rate, int players) {
    ReplayHeader h;
    h.version = kReplayVersion;
    h.tick_rate = static_cast<std::uint16_t>(std::clamp(tick_rate, 1, 65535));
    h.seed = seed;
    h.players = static_cast<std::uint8_t>(std::clamp(players, 1, kMaxPlayers));
    h.build_id = std::string(build_id());
    h.os = os_description();
    return h;
}

Status replay_compatible(const ReplayHeader &h, int tick_rate, bool any_build) {
    if (h.version != kReplayVersion)
        return make_error(ErrorCode::Unsupported, std::format("replay version {} (this build reads {})", h.version,
                                                              kReplayVersion));
    if (static_cast<int>(h.tick_rate) != tick_rate)
        return make_error(ErrorCode::Invalid,
                          std::format("replay tick rate {} differs from the app's {}", h.tick_rate, tick_rate));
    if (any_build) return {};
    if (h.build_id != build_id())
        return make_error(ErrorCode::Unsupported,
                          std::format("replay was recorded by build '{}', this is build '{}' (pass "
                                      "--replay-any-build to try anyway)",
                                      h.build_id, build_id()));
    if (h.os != os_description())
        return make_error(ErrorCode::Unsupported,
                          std::format("replay was recorded on '{}', this is '{}' (pass --replay-any-build to "
                                      "try anyway)",
                                      h.os, os_description()));
    return {};
}

// ---- InputRecorder -----------------------------------------------------------------------------

InputRecorder::InputRecorder() = default;

InputRecorder::~InputRecorder() {
    if (Status s = close(); !s) LAM_LOG_ERROR("input recorder: {}", s.error().message);
}

Status InputRecorder::open(const std::string &path, const ReplayHeader &header) {
    if (is_open()) {
        if (Status s = close(); !s) LAM_LOG_WARN("input recorder: {}", s.error().message);
    }
    if (header.players < 1 || header.players > kMaxPlayers)
        return make_error(ErrorCode::Invalid, std::format("{}: replay players must be 1..{}", path, kMaxPlayers));
    if (header.build_id.size() > 0xFFFF || header.os.size() > 0xFFFF)
        return make_error(ErrorCode::Invalid, std::format("{}: replay header strings too long", path));
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return make_error(ErrorCode::Io, std::format("{}: cannot create replay file ({})", path, std::strerror(errno)));
    std::vector<std::byte> head;
    head.insert(head.end(), reinterpret_cast<const std::byte *>(kMagic), reinterpret_cast<const std::byte *>(kMagic) + 4);
    put_u16(head, header.version);
    put_u16(head, header.tick_rate);
    put_u64(head, header.seed);
    head.push_back(static_cast<std::byte>(header.players));
    put_str(head, header.build_id);
    put_str(head, header.os);
    if (std::fwrite(head.data(), 1, head.size(), f) != head.size()) {
        std::fclose(f);
        return make_error(ErrorCode::Io, std::format("{}: cannot write the replay header", path));
    }
    file_ = f;
    header_ = header;
    path_ = path;
    ticks_ = 0;
    last_tick_ = 0;
    failed_ = false;
    buffer_.clear();
    buffer_.reserve(kFlushBytes + 8 + sizeof(InputFrame) * kMaxPlayers);
    return {};
}

void InputRecorder::record(std::uint64_t app_tick, std::span<const InputFrame, kMaxPlayers> frames) {
    if (file_ == nullptr) return;
    LAM_ASSERT(ticks_ == 0 || app_tick > last_tick_, "InputRecorder::record: ticks must increase");
    const std::size_t players = header_.players;
    const std::size_t old = buffer_.size();
    buffer_.resize(old + 8 + players * sizeof(InputFrame));     // within the reserved capacity
    std::byte *out = buffer_.data() + old;
    put_u64(out, app_tick);
    out += 8;
    for (std::size_t p = 0; p < players; ++p, out += sizeof(InputFrame))
        detail::canonical_frame_bytes(frames[p], out);
    ++ticks_;
    last_tick_ = app_tick;
    if (buffer_.size() >= kFlushBytes) (void)flush();
}

Status InputRecorder::flush() {
    if (file_ == nullptr || buffer_.empty()) return {};
    if (std::fwrite(buffer_.data(), 1, buffer_.size(), file_) != buffer_.size()) failed_ = true;
    buffer_.clear();
    if (failed_) return make_error(ErrorCode::Io, std::format("{}: replay write failed", path_));
    return {};
}

Status InputRecorder::close() {
    if (file_ == nullptr) return {};
    (void)flush();
    if (std::fflush(file_) != 0) failed_ = true;
    if (std::fclose(file_) != 0) failed_ = true;
    file_ = nullptr;
    if (failed_) return make_error(ErrorCode::Io, std::format("{}: replay write failed", path_));
    return {};
}

bool InputRecorder::is_open() const {
    return file_ != nullptr;
}

std::uint64_t InputRecorder::ticks_recorded() const {
    return ticks_;
}

const ReplayHeader &InputRecorder::header() const {
    return header_;
}

// ---- ReplayDriver ------------------------------------------------------------------------------

Result<std::unique_ptr<ReplayDriver>> ReplayDriver::open(const std::string &path) {
    auto bytes = read_file_bytes(path);
    if (!bytes) return bytes.error();
    Reader r{*bytes};
    if (!r.need(4) || std::memcmp(bytes->data(), kMagic, 4) != 0)
        return make_error(ErrorCode::Parse, std::format("{}: not a replay file (bad magic)", path));
    r.pos = 4;
    std::unique_ptr<ReplayDriver> d(new ReplayDriver());
    ReplayHeader &h = d->header_;
    h.version = static_cast<std::uint16_t>(r.uint(2));
    if (r.ok && h.version > kReplayVersion)
        return make_error(ErrorCode::VersionTooNew,
                          std::format("{}: replay version {} is newer than {}", path, h.version, kReplayVersion));
    if (r.ok && h.version < 1)
        return make_error(ErrorCode::Invalid, std::format("{}: replay version {} is invalid", path, h.version));
    h.tick_rate = static_cast<std::uint16_t>(r.uint(2));
    h.seed = r.uint(8);
    h.players = static_cast<std::uint8_t>(r.uint(1));
    h.build_id = r.str();
    h.os = r.str();
    if (!r.ok) return make_error(ErrorCode::Parse, std::format("{}: truncated replay header", path));
    if (h.players < 1 || h.players > kMaxPlayers)
        return make_error(ErrorCode::Invalid, std::format("{}: replay players {} outside 1..{}", path, h.players,
                                                          kMaxPlayers));
    if (h.tick_rate == 0) return make_error(ErrorCode::Invalid, std::format("{}: replay tick rate 0", path));
    d->record_size_ = 8 + static_cast<std::size_t>(h.players) * sizeof(InputFrame);
    const std::size_t body = bytes->size() - r.pos;
    d->count_ = body / d->record_size_;
    if (body % d->record_size_ != 0)
        LAM_LOG_WARN("{}: replay ends with a partial record ({} bytes); ignoring it", path, body % d->record_size_);
    d->data_.assign(bytes->begin() + static_cast<std::ptrdiff_t>(r.pos),
                    bytes->begin() + static_cast<std::ptrdiff_t>(r.pos + d->count_ * d->record_size_));
    for (std::size_t i = 1; i < d->count_; ++i) {
        if (read_u64(d->data_.data() + i * d->record_size_) <= read_u64(d->data_.data() + (i - 1) * d->record_size_))
            return make_error(ErrorCode::Parse, std::format("{}: replay ticks are not increasing at record {}", path, i));
    }
    // fill() memcpy's records into InputFrames, so reject bad intent counts and bools here
    // rather than hit UB later.
    for (std::size_t i = 0; i < d->count_; ++i) {
        for (std::size_t p = 0; p < h.players; ++p) {
            const std::byte *f = d->data_.data() + i * d->record_size_ + 8 + p * sizeof(InputFrame);
            const auto intents = std::to_integer<unsigned>(f[offsetof(InputFrame, intent_count)]);
            const auto valid = std::to_integer<unsigned>(f[offsetof(InputFrame, pointer_valid)]);
            if (intents > static_cast<unsigned>(kMaxIntents) || valid > 1u)
                return make_error(ErrorCode::Parse,
                                  std::format("{}: replay record {} (player {}) is corrupt", path, i, p));
        }
    }
    return d;
}

const ReplayHeader &ReplayDriver::header() const {
    return header_;
}

bool ReplayDriver::finished() const {
    return cursor_ >= count_;
}

std::size_t ReplayDriver::tick_count() const {
    return count_;
}

std::uint64_t ReplayDriver::first_tick() const {
    return count_ > 0 ? read_u64(data_.data()) : 0;
}

std::uint64_t ReplayDriver::last_tick() const {
    return count_ > 0 ? read_u64(data_.data() + (count_ - 1) * record_size_) : 0;
}

void ReplayDriver::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    const std::uint64_t now = ctx.app().tick();
    while (cursor_ < count_ && read_u64(data_.data() + cursor_ * record_size_) < now)
        ++cursor_;
    const bool have = cursor_ < count_ && read_u64(data_.data() + cursor_ * record_size_) == now;
    const std::byte *rec = have ? data_.data() + cursor_ * record_size_ + 8 : nullptr;
    for (std::size_t p = 0; p < frames.size(); ++p) {
        if (rec != nullptr && p < header_.players) std::memcpy(&frames[p], rec + p * sizeof(InputFrame), sizeof(InputFrame));
        else frames[p] = InputFrame{};
    }
    if (have) ++cursor_;
}

} // namespace lam
