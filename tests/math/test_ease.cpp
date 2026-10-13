// Easing curves and their names.
#include <doctest/doctest.h>

#include <lam/math/ease.h>

using namespace lam;

namespace {
constexpr int kEaseCount = static_cast<int>(Ease::Step) + 1;
Ease ease_at(int i) {
    return static_cast<Ease>(i);
}
} // namespace

TEST_CASE("easing curves start at 0, end at 1 and clamp outside") {
    for (int i = 0; i < kEaseCount; ++i) {
        const Ease e = ease_at(i);
        CAPTURE(ease_name(e));
        CHECK(ease(e, 0.0f) == doctest::Approx(0.0f).epsilon(0.001));
        CHECK(ease(e, 1.0f) == doctest::Approx(1.0f).epsilon(0.001));
        CHECK(ease(e, -3.0f) == ease(e, 0.0f));
        CHECK(ease(e, 7.0f) == ease(e, 1.0f));
        for (int k = 0; k <= 20; ++k) {
            const float t = static_cast<float>(k) / 20.0f;
            REQUIRE(ease_fn(e)(t) == ease(e, t));
        }
    }
    CHECK(ease(Ease::OutBack, 0.7f) > 1.0f); // it overshoots
    CHECK(ease(Ease::InBack, 0.3f) < 0.0f);
    CHECK(ease(Ease::Linear, 0.3f) == doctest::Approx(0.3f));
    CHECK(ease(Ease::InQuad, 0.5f) == doctest::Approx(0.25f));
    CHECK(ease(Ease::OutQuad, 0.5f) == doctest::Approx(0.75f));
    CHECK(ease(Ease::InOutQuad, 0.5f) == doctest::Approx(0.5f));
    CHECK(ease(Ease::Step, 0.99f) == 0.0f);
}

TEST_CASE("easing: the monotone curves never go backwards") {
    for (Ease e : {Ease::Linear, Ease::InQuad, Ease::OutQuad, Ease::InOutQuad, Ease::InCubic,
                   Ease::OutCubic, Ease::InOutCubic, Ease::InSine, Ease::OutSine, Ease::InOutSine,
                   Ease::InExpo, Ease::OutExpo, Ease::InOutExpo}) {
        CAPTURE(ease_name(e));
        float prev = ease(e, 0.0f);
        for (int k = 1; k <= 200; ++k) {
            const float v = ease(e, static_cast<float>(k) / 200.0f);
            REQUIRE(v >= prev - 1e-6f);
            prev = v;
        }
    }
}

TEST_CASE("easing: names round-trip") {
    for (int i = 0; i < kEaseCount; ++i) {
        const Ease e = ease_at(i);
        const std::string_view name = ease_name(e);
        REQUIRE_FALSE(name.empty());
        const auto back = ease_from_name(name);
        REQUIRE(back.has_value());
        CHECK(*back == e);
    }
    CHECK(ease_from_name("out_quad") == Ease::OutQuad);
    CHECK(ease_from_name("in_out_sine") == Ease::InOutSine);
    CHECK(ease_name(Ease::OutBounce) == "out_bounce");
    CHECK_FALSE(ease_from_name("nope").has_value());
    CHECK_FALSE(ease_from_name("").has_value());
    CHECK_FALSE(ease_from_name("OutQuad").has_value());
    CHECK(ease_name(static_cast<Ease>(200)).empty());
    CHECK(ease_fn(static_cast<Ease>(200))(0.3f) == doctest::Approx(0.3f)); // falls back to linear
}
