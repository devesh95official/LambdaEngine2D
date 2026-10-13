// FloatTween and Fader.
#include <doctest/doctest.h>

#include <limits>

#include <lam/math/tween.h>

using namespace lam;

TEST_CASE("a tween reaches its end and stays there") {
    FloatTween t;
    t.start(10.0f, 20.0f, 1.0f, Ease::Linear);
    CHECK(t.value() == doctest::Approx(10.0f));
    CHECK_FALSE(t.done());
    t.update(0.25f);
    CHECK(t.value() == doctest::Approx(12.5f));
    t.update(5.0f);
    CHECK(t.done());
    CHECK(t.value() == 20.0f);
    t.update(1.0f);
    CHECK(t.value() == 20.0f);
    // Retarget from where it stands.
    t.start(t.value(), 0.0f, 0.5f, Ease::Linear);
    CHECK(t.value() == doctest::Approx(20.0f));
    t.update(0.5f);
    CHECK(t.value() == 0.0f);
    CHECK(t.done());
    FloatTween instant;
    instant.start(1.0f, 5.0f, 0.0f);
    CHECK(instant.value() == 5.0f);
    CHECK(instant.done());
    instant.start(1.0f, 5.0f, -2.0f);
    CHECK(instant.value() == 5.0f);
}

TEST_CASE("tween: easing, defaults and bad dt") {
    FloatTween t;
    CHECK(t.done()); // a default tween is finished at 0
    CHECK(t.value() == 0.0f);
    t.start(0.0f, 1.0f, 2.0f); // OutQuad by default
    t.update(1.0f);
    CHECK(t.value() == doctest::Approx(0.75f));
    t.update(-1.0f);
    t.update(std::numeric_limits<float>::quiet_NaN());
    t.update(std::numeric_limits<float>::infinity());
    CHECK(t.value() == doctest::Approx(0.75f));
    t.start(0.0f, 1.0f, std::numeric_limits<float>::infinity());
    CHECK(t.value() == 1.0f); // a non-finite duration finishes at once
}

TEST_CASE("a fader moves at a constant rate and settles exactly") {
    Fader f;
    f.snap(0.0f);
    f.set_target(1.0f, 2.0f);
    CHECK_FALSE(f.settled());
    f.update(0.25f);
    CHECK(f.value() == doctest::Approx(0.5f));
    f.update(10.0f);
    CHECK(f.value() == 1.0f);
    CHECK(f.settled());
    f.set_target(0.2f, 0.0f); // rate 0 jumps
    CHECK(f.value() == doctest::Approx(0.2f));
    CHECK(f.settled());
    f.set_target(-1.0f, 1.0f);
    for (int i = 0; i < 100; ++i)
        f.update(1.0f / 60.0f);
    CHECK(f.value() == -1.0f);
}

TEST_CASE("fader: snap, bad dt and bad targets") {
    Fader f;
    f.snap(3.0f);
    CHECK(f.value() == 3.0f);
    CHECK(f.settled());
    f.snap(std::numeric_limits<float>::quiet_NaN());
    CHECK(f.value() == 3.0f);
    f.set_target(5.0f, 1.0f);
    f.update(-1.0f);
    f.update(std::numeric_limits<float>::quiet_NaN());
    CHECK(f.value() == 3.0f);
    f.update(0.5f);
    CHECK(f.value() == doctest::Approx(3.5f));
    f.set_target(std::numeric_limits<float>::quiet_NaN(), 1.0f); // keeps the old target
    f.update(10.0f);
    CHECK(f.value() == 5.0f);
}
