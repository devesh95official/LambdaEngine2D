// ColorCurve: keys interpolate and t wraps at midnight (t = 1 is t = 0).
#include <doctest/doctest.h>

#include <limits>

#include <lam/math/curve.h>

using namespace lam;

TEST_CASE("the ambient curve interpolates and wraps at midnight") {
    // Black at midnight, warm at noon.
    ColorCurve curve;
    curve.add(0.0f, Color{0, 0, 0, 255});
    curve.add(0.5f, Color{200, 100, 50, 255});
    CHECK(curve.size() == 2);
    CHECK(curve.sample(0.25f) == Color{100, 50, 25, 255});
    CHECK(curve.sample(0.75f) == Color{100, 50, 25, 255}); // back down toward midnight
    CHECK(curve.sample(0.5f + 1.0f) == Color{200, 100, 50, 255});
    CHECK(curve.sample(0.0f) == Color{0, 0, 0, 255});
    CHECK(curve.sample(1.0f) == Color{0, 0, 0, 255});
    CHECK(curve.sample(-0.25f) == Color{100, 50, 25, 255});
}

TEST_CASE("color curve: wrapping between the last and the first key") {
    ColorCurve curve;
    curve.add(0.75f, Color{0, 0, 0, 255}); // added out of order: kept sorted
    curve.add(0.25f, Color{200, 200, 200, 255});
    // At 0.0 we are halfway from the 0.75 key (through midnight) to the 0.25 key.
    CHECK(curve.sample(0.0f) == Color{100, 100, 100, 255});
    CHECK(curve.sample(0.25f) == Color{200, 200, 200, 255});
    CHECK(curve.sample(0.5f) == Color{100, 100, 100, 255});
    CHECK(curve.sample(0.75f) == Color{0, 0, 0, 255});
    CHECK(curve.sample(0.9f) == lerp(Color{0, 0, 0, 255}, Color{200, 200, 200, 255}, 0.3f));
}

TEST_CASE("color curve: edge cases") {
    ColorCurve empty;
    CHECK(empty.size() == 0);
    CHECK(empty.sample(0.3f) == kWhite);
    ColorCurve one;
    one.add(0.4f, Color{1, 2, 3, 4});
    CHECK(one.sample(0.9f) == Color{1, 2, 3, 4});
    CHECK(one.sample(std::numeric_limits<float>::quiet_NaN()) == Color{1, 2, 3, 4});
    ColorCurve wrapped;
    wrapped.add(1.25f, Color{10, 10, 10, 255});  // -> 0.25
    wrapped.add(-0.25f, Color{30, 30, 30, 255}); // -> 0.75
    CHECK(wrapped.sample(0.25f) == Color{10, 10, 10, 255});
    CHECK(wrapped.sample(0.75f) == Color{30, 30, 30, 255});
    wrapped.add(std::numeric_limits<float>::quiet_NaN(), kBlack); // ignored
    CHECK(wrapped.size() == 2);
    ColorCurve full;
    for (int i = 0; i < 20; ++i)
        full.add(static_cast<float>(i) / 20.0f, kBlack);
    CHECK(full.size() == 16); // later keys are ignored
}
