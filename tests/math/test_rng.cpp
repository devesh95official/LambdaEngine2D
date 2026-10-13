// Rng (PCG32).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <span>

#include <lam/math/rng.h>

using lam::Rng;

namespace {

constexpr std::uint32_t third_of(std::uint64_t seed) {
    Rng rng(seed);
    rng.next_u32();
    rng.next_u32();
    return rng.next_u32();
}

} // namespace

// Integer-only, so it also runs at compile time and gives the same numbers everywhere.
static_assert(third_of(7) == third_of(7));
static_assert(third_of(7) != third_of(8));
static_assert(lam::derive_seed(1, 2) == lam::derive_seed(1, 2));
static_assert(lam::derive_seed(1, 2) != lam::derive_seed(1, 3));

TEST_CASE("rng: the same seed gives the same sequence, another seed another one") {
    Rng a(1234);
    Rng b(1234);
    Rng c(1235);
    int different = 0;
    for (int i = 0; i < 1000; ++i) {
        const std::uint32_t value = a.next_u32();
        CHECK(value == b.next_u32());
        if (value != c.next_u32()) ++different;
    }
    CHECK(different > 990);
    CHECK(a.state() == b.state());

    Rng built_at_run_time(7);
    built_at_run_time.next_u32();
    built_at_run_time.next_u32();
    CHECK(built_at_run_time.next_u32() == third_of(7));
}

TEST_CASE("rng: reseeding starts the sequence again") {
    Rng rng(99);
    const std::uint32_t first = rng.next_u32();
    const std::uint32_t second = rng.next_u32();
    rng.reseed(99);
    CHECK(rng.next_u32() == first);
    CHECK(rng.next_u32() == second);
    CHECK(Rng().state() == Rng(0).state());
}

TEST_CASE("rng: below() stays under its bound and reaches every value") {
    Rng rng(5);
    CHECK(rng.below(0) == 0);
    CHECK(rng.below(1) == 0);
    int seen[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 6000; ++i) {
        const std::uint32_t value = rng.below(6);
        REQUIRE(value < 6);
        ++seen[value];
    }
    for (int count : seen) {
        CHECK(count > 800); // 1000 expected each
        CHECK(count < 1200);
    }
}

TEST_CASE("rng: between() includes both ends; a reversed range gives the low end") {
    Rng rng(11);
    bool low = false;
    bool high = false;
    for (int i = 0; i < 2000; ++i) {
        const int value = rng.between(-3, 4);
        REQUIRE(value >= -3);
        REQUIRE(value <= 4);
        if (value == -3) low = true;
        if (value == 4) high = true;
    }
    CHECK(low);
    CHECK(high);
    CHECK(rng.between(9, 9) == 9);
    CHECK(rng.between(9, 2) == 9);
}

TEST_CASE("rng: unit() is in [0, 1)") {
    Rng rng(3);
    float sum = 0.0f;
    for (int i = 0; i < 10000; ++i) {
        const float value = rng.unit();
        REQUIRE(value >= 0.0f);
        REQUIRE(value < 1.0f);
        sum += value;
    }
    CHECK(sum / 10000.0f == doctest::Approx(0.5f).epsilon(0.05));
}

TEST_CASE("rng: between() covers the full int range without overflow") {
    Rng rng(21);
    bool negative = false;
    bool positive = false;
    for (int i = 0; i < 2000; ++i) {
        const int v = rng.between(INT_MIN, INT_MAX);
        if (v < 0) negative = true;
        if (v > 0) positive = true;
    }
    CHECK(negative);
    CHECK(positive);
    for (int i = 0; i < 2000; ++i) {
        const int a = rng.between(0, INT_MAX);
        REQUIRE(a >= 0);
        const int b = rng.between(INT_MIN, 0);
        REQUIRE(b <= 0);
        const int c = rng.between(INT_MIN, INT_MIN + 1);
        REQUIRE((c == INT_MIN || c == INT_MIN + 1));
        const int d = rng.between(INT_MAX - 1, INT_MAX);
        REQUIRE((d == INT_MAX - 1 || d == INT_MAX));
        const int e = rng.between(-1000000000, 2000000000); // span > INT_MAX
        REQUIRE(e >= -1000000000);
        REQUIRE(e <= 2000000000);
    }
    CHECK(rng.between(INT_MAX, INT_MIN) == INT_MAX);
}

TEST_CASE("rng: range() never returns hi, even when float rounding would") {
    Rng rng(8);
    // At 1e8 the float spacing is 8, so lo + 8 * unit() rounds up to hi for unit() near 1.
    const float lo = 1e8f;
    const float hi = 1e8f + 8.0f;
    for (int i = 0; i < 20000; ++i) {
        const float v = rng.range(lo, hi);
        REQUIRE(v >= lo);
        REQUIRE(v < hi);
    }
    for (int i = 0; i < 20000; ++i) {
        const float v = rng.range(-hi, -lo);
        REQUIRE(v >= -hi);
        REQUIRE(v < -lo);
    }
    const float tiny_hi = std::nextafter(1.0f, 2.0f);
    for (int i = 0; i < 1000; ++i)
        REQUIRE(rng.range(1.0f, tiny_hi) == 1.0f);
    for (int i = 0; i < 1000; ++i) {
        const float v = rng.range(-1.0f, 0.0f);
        REQUIRE(v >= -1.0f);
        REQUIRE(v < 0.0f);
    }
    // Empty or reversed ranges give lo.
    CHECK(rng.range(5.0f, 5.0f) == 5.0f);
    CHECK(rng.range(5.0f, 1.0f) == 5.0f);
    CHECK(rng.range(2.0f, std::nanf("")) == 2.0f);
}

TEST_CASE("rng: chance(), unit_vector() and in_disc()") {
    Rng rng(12);
    int hits = 0;
    for (int i = 0; i < 1000; ++i) {
        REQUIRE_FALSE(rng.chance(0.0f));
        REQUIRE_FALSE(rng.chance(-1.0f));
        REQUIRE(rng.chance(1.0f));
        if (rng.chance(0.25f)) ++hits;
        const lam::Vec2 u = rng.unit_vector();
        REQUIRE(lam::length(u) == doctest::Approx(1.0f).epsilon(1e-5));
        const lam::Vec2 d = rng.in_disc(3.0f);
        REQUIRE(lam::length(d) <= 3.0f + 1e-4f);
    }
    CHECK(hits > 200);
    CHECK(hits < 300);
}

TEST_CASE("rng: shuffle is a deterministic permutation") {
    std::array<int, 20> a{};
    for (int i = 0; i < 20; ++i)
        a[static_cast<std::size_t>(i)] = i;
    std::array<int, 20> b = a;
    std::array<int, 20> c = a;
    Rng ra(42), rb(42), rc(43);
    ra.shuffle(std::span<int>(a));
    rb.shuffle(std::span<int>(b));
    rc.shuffle(std::span<int>(c));
    CHECK(a == b);
    CHECK(a != c);
    std::array<int, 20> sorted = a;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < 20; ++i)
        CHECK(sorted[static_cast<std::size_t>(i)] == i);
    std::array<int, 1> one{7};
    ra.shuffle(std::span<int>(one));
    CHECK(one[0] == 7);
    ra.shuffle(std::span<int>{});
}

TEST_CASE("rng: state() and set_state() restore the sequence; streams are independent") {
    Rng rng(1);
    rng.next_u32();
    const std::uint64_t saved = rng.state();
    const std::uint32_t a = rng.next_u32();
    const std::uint32_t b = rng.next_u32();
    rng.set_state(saved);
    CHECK(rng.next_u32() == a);
    CHECK(rng.next_u32() == b);

    // derive_seed spreads streams: neighbouring streams do not share their first values.
    Rng s1(lam::derive_seed(100, 1));
    Rng s2(lam::derive_seed(100, 2));
    int same = 0;
    for (int i = 0; i < 100; ++i)
        if (s1.next_u32() == s2.next_u32()) ++same;
    CHECK(same < 3);
    CHECK(lam::derive_seed(0, 0) != 0);
}
