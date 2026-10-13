#include <lam/core/hash.h>

#include <bit>
#include <cmath>
#include <cstring>

namespace lam {
namespace {
constexpr std::uint64_t kP1 = 0x9E3779B185EBCA87ULL;
constexpr std::uint64_t kP2 = 0xC2B2AE3D27D4EB4FULL;
std::uint64_t rotl(std::uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
std::uint64_t mix(std::uint64_t h, std::uint64_t k) {
    k *= kP2;
    k = rotl(k, 31);
    k *= kP1;
    h ^= k;
    return rotl(h, 27) * kP1 + 0x52DCE729ULL;
}
} // namespace

void Hasher::bytes(const void *data, std::size_t size) {
    const auto *p = static_cast<const unsigned char *>(data);
    length_ += size;
    while (size >= 8) {
        std::uint64_t k;
        std::memcpy(&k, p, 8);
        state_ = mix(state_, k);
        p += 8;
        size -= 8;
    }
    if (size > 0) {
        std::uint64_t k = 0;
        std::memcpy(&k, p, size);
        state_ = mix(state_, k ^ (static_cast<std::uint64_t>(size) << 56));
    }
}

void Hasher::u64(std::uint64_t v) {
    length_ += 8;
    state_ = mix(state_, v);
}

void Hasher::f32(float v) {
    if (v == 0.0f) v = 0.0f;                                    // -0 -> +0
    std::uint32_t bits = std::isnan(v) ? 0x7FC00000u : std::bit_cast<std::uint32_t>(v);
    u64(bits);
}

void Hasher::f64(double v) {
    if (v == 0.0) v = 0.0;
    std::uint64_t bits = std::isnan(v) ? 0x7FF8000000000000ULL : std::bit_cast<std::uint64_t>(v);
    u64(bits);
}

std::uint64_t Hasher::value() const {
    std::uint64_t h = state_ ^ length_;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 33;
    return h;
}

} // namespace lam
