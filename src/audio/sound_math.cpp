// spatial_mix (from the old AudioSystem::play_at) and sound lengths read from file headers.
// Malformed files give 0.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include <lam/audio/audio.h>
#include <lam/math/scalar.h>
#include <lam/math/vec2.h>

namespace lam {

SpatialMix spatial_mix(Vec2 listener, Vec2 source, float range) {
    const float dx = source.x - listener.x;
    const float dy = source.y - listener.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (!(range > 0.0f) || !std::isfinite(range) || !(d < range)) return {0.0f, 0.0f};
    const float near = range * 0.25f;
    const float fall = d <= near ? 1.0f : 1.0f - (d - near) / (range - near);
    const float pan = clamp(dx / range, -0.8f, 0.8f);
    return {fall * fall, pan};
}

namespace {

using Bytes = std::span<const std::byte>;

std::uint32_t u8_at(Bytes b, std::size_t at) { return std::to_integer<std::uint32_t>(b[at]); }

bool has(Bytes b, std::size_t at, std::size_t n) { return at <= b.size() && n <= b.size() - at; }

std::uint32_t le16(Bytes b, std::size_t at) { return u8_at(b, at) | (u8_at(b, at + 1) << 8); }

std::uint32_t le32(Bytes b, std::size_t at) {
    return u8_at(b, at) | (u8_at(b, at + 1) << 8) | (u8_at(b, at + 2) << 16) | (u8_at(b, at + 3) << 24);
}

std::uint64_t le64(Bytes b, std::size_t at) {
    return static_cast<std::uint64_t>(le32(b, at)) | (static_cast<std::uint64_t>(le32(b, at + 4)) << 32);
}

std::uint32_t be24(Bytes b, std::size_t at) {
    return (u8_at(b, at) << 16) | (u8_at(b, at + 1) << 8) | u8_at(b, at + 2);
}

std::uint32_t be32(Bytes b, std::size_t at) { return (u8_at(b, at) << 24) | be24(b, at + 1); }

std::uint64_t be64(Bytes b, std::size_t at) {
    return (static_cast<std::uint64_t>(be32(b, at)) << 32) | be32(b, at + 4);
}

bool tag(Bytes b, std::size_t at, const char *t) {
    const std::size_t n = std::strlen(t);
    if (!has(b, at, n)) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (static_cast<char>(b[at + i]) != t[i]) return false;
    return true;
}

float wav_seconds(Bytes b) {
    if (!tag(b, 0, "RIFF") || !tag(b, 8, "WAVE")) return 0;
    std::size_t at = 12;
    std::uint32_t byte_rate = 0;
    while (has(b, at, 8)) {
        const std::uint32_t size = le32(b, at + 4);
        const std::size_t body = at + 8;
        if (tag(b, at, "fmt ")) {
            if (!has(b, body, 16)) return 0;
            byte_rate = le32(b, body + 8);
        } else if (tag(b, at, "data")) {
            if (byte_rate == 0) return 0;
            const std::size_t available = b.size() - body;
            const std::size_t bytes = std::min<std::size_t>(size, available); // streaming writers lie
            return static_cast<float>(static_cast<double>(bytes) / byte_rate);
        }
        const std::size_t next = body + size + (size & 1u);
        if (next <= at) return 0;
        at = next;
    }
    return 0;
}

float ogg_seconds(Bytes b) {
    if (!tag(b, 0, "OggS") || !has(b, 0, 27)) return 0;
    const std::size_t segments = u8_at(b, 26);
    const std::size_t packet = 27 + segments;
    double rate = 0;
    std::uint64_t skip = 0;
    if (has(b, packet, 16) && u8_at(b, packet) == 1 && tag(b, packet + 1, "vorbis")) {
        rate = le32(b, packet + 12);
    } else if (has(b, packet, 16) && tag(b, packet, "OpusHead")) {
        rate = 48000; // Opus granules count 48 kHz samples
        skip = le16(b, packet + 10);
    }
    if (!(rate > 0)) return 0;
    for (std::size_t at = b.size() >= 27 ? b.size() - 27 : 0;; --at) { // last page wins
        if (tag(b, at, "OggS") && has(b, at, 14)) {
            const std::uint64_t granule = le64(b, at + 6);
            if (granule == ~std::uint64_t{0} || granule < skip) return 0;
            return static_cast<float>(static_cast<double>(granule - skip) / rate);
        }
        if (at == 0) break;
    }
    return 0;
}

float qoa_seconds(Bytes b) {
    if (!tag(b, 0, "qoaf") || !has(b, 8, 4)) return 0;
    const std::uint32_t samples = be32(b, 4);
    const std::uint32_t rate = be24(b, 9);
    return rate > 0 ? static_cast<float>(static_cast<double>(samples) / rate) : 0.0f;
}

float flac_seconds(Bytes b) {
    // "fLaC", then a metadata block header (type 0 = STREAMINFO, 34 bytes). Sample rate (20 bits)
    // and total samples (36 bits) sit in the 8 bytes starting 10 bytes into STREAMINFO.
    if (!tag(b, 0, "fLaC") || !has(b, 8, 18) || (u8_at(b, 4) & 0x7Fu) != 0) return 0;
    const std::uint64_t v = be64(b, 18);
    const auto rate = static_cast<std::uint32_t>(v >> 44);
    const std::uint64_t total = v & 0xFFFFFFFFFULL;
    return rate > 0 ? static_cast<float>(static_cast<double>(total) / rate) : 0.0f;
}

} // namespace

float encoded_sound_seconds(std::span<const std::byte> bytes, std::string_view ext) {
    if (ext == ".wav") return wav_seconds(bytes);
    if (ext == ".ogg") return ogg_seconds(bytes);
    if (ext == ".qoa") return qoa_seconds(bytes);
    if (ext == ".flac") return flac_seconds(bytes);
    return 0;
}

} // namespace lam
