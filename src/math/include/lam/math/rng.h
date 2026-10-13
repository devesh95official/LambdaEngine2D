#pragma once
// PCG32 seeded through SplitMix64. Integer maths only, so a seed gives the same sequence
// everywhere. Give each system its own stream (World::rng) so one can't shift another's draws.
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <lam/math/vec2.h>
namespace lam {
class Rng {
public:
    explicit constexpr Rng(std::uint64_t seed = 0) { reseed(seed); }
    constexpr void reseed(std::uint64_t seed) {
        state_ = 0;
        next_u32();
        state_ += seed;
        next_u32();
    }
    constexpr std::uint32_t next_u32() {
        const std::uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + kIncrement;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18U) ^ old) >> 27U);
        const auto rotation = static_cast<std::uint32_t>(old >> 59U);
        return (xorshifted >> rotation) | (xorshifted << ((32U - rotation) & 31U));
    }
    // [0, bound), without modulo bias. bound 0 -> 0.
    constexpr std::uint32_t below(std::uint32_t bound) {
        if (bound == 0) return 0;
        const std::uint32_t threshold = (0U - bound) % bound;
        for (;;) {
            const std::uint32_t value = next_u32();
            if (value >= threshold) return value % bound;
        }
    }
    // [lo, hi], both inclusive; lo > hi -> lo. Exact for the full int range.
    constexpr int between(int lo, int hi) {
        if (hi <= lo) return lo;
        const std::uint64_t span = static_cast<std::uint64_t>(static_cast<std::int64_t>(hi) - lo) + 1U;
        if (span > 0xFFFFFFFFULL) return static_cast<int>(static_cast<std::int64_t>(lo) + next_u32());
        return static_cast<int>(static_cast<std::int64_t>(lo) + below(static_cast<std::uint32_t>(span)));
    }
    // [0, 1) with 24 bits of resolution.
    constexpr float unit() { return static_cast<float>(next_u32() >> 8U) * (1.0f / 16777216.0f); }
    // [lo, hi); lo >= hi -> lo. Never returns hi, even after float rounding.
    constexpr float range(float lo, float hi) {
        if (!(hi > lo)) return lo;
        const float v = lo + (hi - lo) * unit();
        if (v < hi) return v;
        return below_float(hi, lo);
    }
    // true with probability p (p <= 0 never, p >= 1 always).
    constexpr bool chance(float p) { return unit() < p; }
    // A random unit vector (det_sin/det_cos).
    Vec2 unit_vector();
    // Uniform point in a disc of `radius` around the origin.
    Vec2 in_disc(float radius);
    // Fisher-Yates shuffle.
    template <class T> void shuffle(std::span<T> items) {
        for (std::size_t i = items.size(); i > 1; --i) {
            const std::size_t j = below(static_cast<std::uint32_t>(i));
            using std::swap;
            swap(items[i - 1], items[j]);
        }
    }
    // For snapshots and saves: restores the sequence exactly.
    constexpr std::uint64_t state() const { return state_; }
    constexpr void set_state(std::uint64_t s) { state_ = s; }

private:
    // The largest float below `hi` (toward lo), without libm.
    static constexpr float below_float(float hi, float lo) {
        if (hi == 0.0f) return -1.4e-45f > lo ? -1.4e-45f : lo;
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(hi);
        const float r = std::bit_cast<float>(hi > 0.0f ? bits - 1U : bits + 1U);
        return r < lo ? lo : r;
    }
    static constexpr std::uint64_t kIncrement = 1442695040888963407ULL;
    std::uint64_t state_ = 0;
};
// An independent seed from a base seed (SplitMix64 finaliser); `stream` picks which one.
constexpr std::uint64_t derive_seed(std::uint64_t base, std::uint64_t stream) {
    std::uint64_t z = base + 0x9E3779B97F4A7C15ULL * (stream + 1);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}
}
