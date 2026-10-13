// lam_core Hasher (World::state_hash): deterministic, order sensitive, floats canonicalised
// (-0 hashes as 0, every NaN hashes the same).
#include <doctest/doctest.h>

#include <bit>
#include <cstdint>
#include <limits>

#include <lam/core/hash.h>

using namespace lam;

namespace {
std::uint64_t hash_f32(float v) {
    Hasher h;
    h.f32(v);
    return h.value();
}
std::uint64_t hash_f64(double v) {
    Hasher h;
    h.f64(v);
    return h.value();
}
} // namespace

TEST_CASE("hash: the same calls give the same value") {
    Hasher a, b;
    for (std::uint64_t i = 0; i < 100; ++i) {
        a.u64(i * 7919);
        b.u64(i * 7919);
    }
    a.bytes("hello", 5);
    b.bytes("hello", 5);
    a.f32(1.5f);
    b.f32(1.5f);
    CHECK(a.value() == b.value());
    CHECK(Hasher{}.value() == Hasher{}.value());
}

TEST_CASE("hash: order, values and lengths matter") {
    Hasher a, b;
    a.u64(1);
    a.u64(2);
    b.u64(2);
    b.u64(1);
    CHECK(a.value() != b.value());

    Hasher c, d;
    c.u64(1);
    d.u64(1);
    d.u64(0);
    CHECK(c.value() != d.value()); // an extra zero changes the hash

    Hasher e, f;
    e.bytes("a", 1);
    f.bytes("a\0", 2);
    CHECK(e.value() != f.value());

    Hasher g, h;
    g.bytes("abcdefghij", 10);
    h.bytes("abcdefghik", 10);
    CHECK(g.value() != h.value());
    CHECK(Hasher{}.value() != c.value());
}

TEST_CASE("hash: floats are canonicalised") {
    CHECK(hash_f32(0.0f) == hash_f32(-0.0f));
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const float neg_nan = std::bit_cast<float>(0xFFC00000u);
    const float payload_nan = std::bit_cast<float>(0x7F800001u);
    CHECK(hash_f32(qnan) == hash_f32(neg_nan));
    CHECK(hash_f32(qnan) == hash_f32(payload_nan));
    CHECK(hash_f32(1.0f) != hash_f32(2.0f));
    CHECK(hash_f32(1.0f) != hash_f32(-1.0f));
    CHECK(hash_f32(std::numeric_limits<float>::infinity()) != hash_f32(qnan));

    CHECK(hash_f64(0.0) == hash_f64(-0.0));
    CHECK(hash_f64(std::numeric_limits<double>::quiet_NaN()) ==
          hash_f64(std::bit_cast<double>(0xFFF8000000000123ULL)));
    CHECK(hash_f64(1.0) != hash_f64(1.0 + 1e-15));
}
