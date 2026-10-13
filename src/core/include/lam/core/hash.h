#pragma once
// Streaming 64-bit hash for World::state_hash. Floats are canonicalised (-0 -> 0, one NaN).
// Not cryptographic; stable for a given sequence of calls.
#include <cstddef>
#include <cstdint>
namespace lam {
class Hasher {
public:
    void bytes(const void *data, std::size_t size);
    void u64(std::uint64_t v);
    void f32(float v);
    void f64(double v);
    std::uint64_t value() const;
private:
    std::uint64_t state_ = 0x243F6A8885A308D3ULL;
    std::uint64_t length_ = 0;
};
}
