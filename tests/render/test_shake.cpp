// Screen shake: add_trauma, decay_trauma and camera_shake_offset. Offsets are whole world pixels
// and the seed gives each camera its own noise.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <set>

#include <lam/render/camera.h>
#include <lam/render/camera_math.h>
#include <lam/test/support.h>

namespace {

lam::CameraShake config() {
    lam::CameraShake c;
    c.decay_per_second = 2.0f;
    c.max_offset = 6.0f;
    c.frequency_hz = 25.0f;
    c.exponent = 2.0f;
    return c;
}

constexpr float kFrame = 1.0f / 60.0f;
constexpr std::uint32_t kSeed = 7;

int ox(const lam::CameraShake &s) {
    return static_cast<int>(lam::camera_shake_offset(s, kSeed).x);
}
int oy(const lam::CameraShake &s) {
    return static_cast<int>(lam::camera_shake_offset(s, kSeed).y);
}

} // namespace

TEST_CASE("shake: trauma adds up, is limited to 1 and ignores nonsense") {
    lam::CameraShake shake = config();
    CHECK(shake.trauma == 0.0f);
    lam::add_trauma(shake, 0.3f);
    lam::add_trauma(shake, 0.25f);
    CHECK(shake.trauma == doctest::Approx(0.55f));
    lam::add_trauma(shake, -1.0f);
    lam::add_trauma(shake, std::numeric_limits<float>::quiet_NaN());
    lam::add_trauma(shake, std::numeric_limits<float>::infinity());
    CHECK(shake.trauma == doctest::Approx(0.55f));
    lam::add_trauma(shake, 0.9f);
    CHECK(shake.trauma == 1.0f);
}

TEST_CASE("shake: trauma decays linearly with real time and stops at 0") {
    lam::CameraShake shake = config();
    lam::add_trauma(shake, 1.0f);
    lam::decay_trauma(shake, 0.25f);
    CHECK(shake.trauma == doctest::Approx(0.5f));
    lam::decay_trauma(shake, 0.0f);
    lam::decay_trauma(shake, -1.0f);
    lam::decay_trauma(shake, std::numeric_limits<float>::quiet_NaN());
    CHECK(shake.trauma == doctest::Approx(0.5f));
    lam::decay_trauma(shake, 10.0f);
    CHECK(shake.trauma == 0.0f);
    CHECK(ox(shake) == 0);
    CHECK(oy(shake) == 0);
}

TEST_CASE("shake: no trauma or no max_offset gives no offset") {
    lam::CameraShake shake = config();
    for (int i = 0; i < 30; ++i) {
        lam::decay_trauma(shake, kFrame);
        CHECK(ox(shake) == 0);
        CHECK(oy(shake) == 0);
    }
    lam::add_trauma(shake, 1.0f);
    shake.max_offset = 0.0f;
    shake.decay_per_second = 0.0f;
    for (int i = 0; i < 30; ++i) {
        lam::decay_trauma(shake, kFrame);
        CHECK(ox(shake) == 0);
        CHECK(oy(shake) == 0);
    }
}

TEST_CASE("shake: the offset is whole pixels, never beyond max_offset, and does move") {
    lam::CameraShake c = config();
    c.decay_per_second = 0.0001f;
    c.max_offset = 6.6f; // not whole: the limit is 6, not round(6.6) = 7
    lam::add_trauma(c, 1.0f);
    std::set<int> seen_x, seen_y;
    int same = 0, big_jumps = 0, previous_x = 0;
    for (int i = 0; i < 600; ++i) {
        lam::decay_trauma(c, kFrame);
        const lam::Vec2 o = lam::camera_shake_offset(c, kSeed);
        CHECK(o.x == std::floor(o.x));
        CHECK(o.y == std::floor(o.y));
        const int x = static_cast<int>(o.x), y = static_cast<int>(o.y);
        CHECK(std::abs(x) <= 6);
        CHECK(std::abs(y) <= 6);
        seen_x.insert(x);
        seen_y.insert(y);
        if (x == y) ++same;
        if (i > 0 && std::abs(x - previous_x) > 9) ++big_jumps;
        previous_x = x;
    }
    CHECK(seen_x.size() >= 8);
    CHECK(seen_y.size() >= 8);
    CHECK(*seen_x.begin() < -2);
    CHECK(*seen_x.rbegin() > 2);
    CHECK(*seen_y.begin() < -2);
    CHECK(*seen_y.rbegin() > 2);
    CHECK(same < 300);
    CHECK(big_jumps == 0);
}

TEST_CASE("shake: small trauma is subtle (the exponent), big trauma is strong") {
    const auto largest = [](float trauma) {
        lam::CameraShake c = config();
        c.decay_per_second = 0.0001f;
        lam::add_trauma(c, trauma);
        int most = 0;
        for (int i = 0; i < 600; ++i) {
            lam::decay_trauma(c, kFrame);
            most = std::max(most, std::abs(ox(c)));
        }
        return most;
    };
    CHECK(largest(0.3f) <= 1);
    CHECK(largest(1.0f) >= 4);
    CHECK(largest(0.3f) < largest(0.7f));
}

TEST_CASE("shake: the same calls give the same shake, and a reset starts it over") {
    lam::CameraShake a = config(), b = config();
    lam::add_trauma(a, 0.8f);
    lam::add_trauma(b, 0.8f);
    for (int i = 0; i < 20; ++i) {
        lam::decay_trauma(a, kFrame);
        lam::decay_trauma(b, kFrame);
        CHECK(ox(a) == ox(b));
        CHECK(oy(a) == oy(b));
    }
    a.trauma = 0.0f; // reset = a fresh component state
    a.time = 0.0f;
    CHECK(ox(a) == 0);
    lam::CameraShake fresh = config();
    lam::add_trauma(a, 0.8f);
    lam::add_trauma(fresh, 0.8f);
    for (int i = 0; i < 20; ++i) {
        lam::decay_trauma(a, kFrame);
        lam::decay_trauma(fresh, kFrame);
        CHECK(ox(a) == ox(fresh));
        CHECK(oy(a) == oy(fresh));
    }
    SUBCASE("different seeds (cameras) shake differently") {
        lam::CameraShake s = config();
        s.decay_per_second = 0.0f;
        lam::add_trauma(s, 1.0f);
        int differ = 0;
        for (int i = 0; i < 120; ++i) {
            lam::decay_trauma(s, kFrame);
            if (lam::camera_shake_offset(s, 1) != lam::camera_shake_offset(s, 2)) ++differ;
        }
        CHECK(differ > 30);
    }
}

TEST_CASE("shake: a very long session keeps working (the noise clock wraps)") {
    lam::CameraShake c = config();
    for (int i = 0; i < 4000; ++i)
        lam::decay_trauma(c, 1.0f);
    CHECK(c.time >= 0.0f);
    CHECK(c.time < 256.0f);
    lam::add_trauma(c, 1.0f);
    c.decay_per_second = 0.0001f;
    std::set<int> seen;
    for (int i = 0; i < 120; ++i) {
        lam::decay_trauma(c, kFrame);
        const int x = ox(c);
        CHECK(std::abs(x) <= 6);
        seen.insert(x);
    }
    CHECK(seen.size() >= 4);
}

TEST_CASE("shake: a clock far outside the noise period wraps instead of overflowing int") {
    // Any finite time is accepted. 1e10 and -1e10 are multiples of the 256-tick noise period, so
    // they must shake like time 0. Casting 1e10 to int would be UB.
    lam::CameraShake shake = config();
    shake.max_offset = 100.0f;
    shake.trauma = 1.0f;
    int differ = 0;
    for (std::uint32_t seed = 0; seed < 16; ++seed) {
        shake.time = 0.0f;
        const lam::Vec2 at0 = lam::camera_shake_offset(shake, seed);
        shake.time = 1e10f;
        differ += lam::camera_shake_offset(shake, seed) == at0 ? 0 : 1;
        shake.time = -1e10f;
        differ += lam::camera_shake_offset(shake, seed) == at0 ? 0 : 1;
    }
    CHECK(differ == 0);
    shake.time = std::numeric_limits<float>::max();
    const lam::Vec2 big = lam::camera_shake_offset(shake, kSeed);
    CHECK(std::abs(big.x) <= 100.0f);
    CHECK(std::abs(big.y) <= 100.0f);
}
