// FixedClock: whole ticks from real time, alpha is the remainder, hitches are capped.
#include <doctest/doctest.h>

#include <limits>

#include <lam/core/clock.h>

using lam::FixedClock;

TEST_CASE("clock: real time is spent in whole ticks and alpha is the leftover") {
    FixedClock clock(60);
    CHECK(clock.advance(1.0 / 120.0) == 0);
    CHECK(clock.alpha() == doctest::Approx(0.5f));
    CHECK(clock.advance(1.0 / 120.0) == 1);
    CHECK(clock.alpha() == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(clock.advance(3.0 / 60.0 + 1e-9) == 3);
    // A 10 s stall is capped at max_ticks_per_frame (default 4).
    CHECK(clock.advance(10.0) == 4);
    CHECK(clock.advance(-1.0) == 0);
    CHECK(clock.ticks_per_second() == 60);
    CHECK(clock.tick_seconds() == doctest::Approx(1.0f / 60.0f));
}

TEST_CASE("clock: max_ticks_per_frame drops the excess instead of banking it") {
    // 64 Hz keeps every number exact in binary.
    FixedClock clock(64, 0.25, 4);
    CHECK(clock.advance(10.0) == 4); // 0.25 s banked = 16 ticks, 4 run, 12 dropped
    CHECK(clock.alpha() == 0.0f);
    CHECK(clock.advance(1.0 / 64.0) == 1); // no catch-up afterwards
    CHECK(clock.advance(1.5 / 64.0) == 1);
    CHECK(clock.alpha() == doctest::Approx(0.5f));
    CHECK(clock.advance(3.0 / 64.0) == 3);
    CHECK(clock.alpha() == doctest::Approx(0.5f));

    FixedClock one(64, 1.0, 1);
    CHECK(one.advance(5.0 / 64.0) == 1);
    CHECK(one.alpha() < 1.0f);
    CHECK(one.advance(1.0 / 64.0) == 1);
}

TEST_CASE("clock: bad input, reset and alpha range") {
    FixedClock clock(50);
    CHECK(clock.advance(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(clock.advance(std::numeric_limits<double>::infinity()) == 0);
    CHECK(clock.advance(0.0) == 0);
    clock.advance(0.013);
    CHECK(clock.alpha() > 0.0f);
    clock.reset();
    CHECK(clock.alpha() == 0.0f);
    for (int i = 0; i < 1000; ++i) {
        clock.advance(0.0071 * (i % 7));
        REQUIRE(clock.alpha() >= 0.0f);
        REQUIRE(clock.alpha() < 1.0f);
    }
    FixedClock odd(0, -1.0, 0); // clamps to 1 Hz, 1 tick per frame
    CHECK(odd.ticks_per_second() == 1);
    CHECK(odd.advance(5.0) <= 1);
}

TEST_CASE("clock: total ticks follow total time at a steady frame rate") {
    FixedClock clock(60);
    int ticks = 0;
    for (int i = 0; i < 600; ++i)
        ticks += clock.advance(1.0 / 144.0);
    CHECK(ticks >= 249);
    CHECK(ticks <= 250); // 600 / 144 s = 4.1667 s = 250 ticks
}
