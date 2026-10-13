// Fading occluders (fade.h). FadeOccluder::top is relative to the occluder's feet.
#include <algorithm>
#include <cmath>
#include <limits>

#include <lam/render/fade.h>
#include <lam/test/support.h>

namespace {

using lam::Rect;
using lam::Vec2;

constexpr float kTreeX = 488.0f;
constexpr float kTreeY = 288.0f;
constexpr float kInset = 4.0f;
// A 48 x 48 top, centred on the feet, 14 px above them.
const lam::FadeOccluder kTree{{-24.0f, -14.0f - 48.0f, 48.0f, 48.0f}, kInset, 0.35f};
const Rect kTop = lam::occluder_rect(kTree, {kTreeX, kTreeY});

bool hidden(float x, float y, float inset = kInset, const Rect &top = kTop) {
    return lam::hidden_behind(top, inset, kTreeY, {x, y});
}

} // namespace

TEST_CASE("fade: the top rectangle sits relative to the occluder's feet") {
    CHECK(kTop.x == 464.0f);
    CHECK(kTop.y == 226.0f);
    CHECK(kTop.w == 48.0f);
    CHECK(kTop.h == 48.0f);
    CHECK(kTop.y + kTop.h == kTreeY - 14.0f);
}

TEST_CASE("fade: hidden only when the feet are behind the object and inside the inset top") {
    CHECK(hidden(488.0f, 256.0f));
    SUBCASE("in front of the object, or level with it, never hides") {
        CHECK_FALSE(hidden(488.0f, 300.0f));
        CHECK_FALSE(hidden(488.0f, kTreeY));
        const Rect low{464.0f, 250.0f, 48.0f, 60.0f};
        CHECK(hidden(488.0f, 287.75f, kInset, low));
        CHECK_FALSE(hidden(488.0f, 288.0f, kInset, low));
        CHECK_FALSE(hidden(488.0f, 295.0f, kInset, low));
    }
    SUBCASE("brushing the left or right edge does not hide") {
        CHECK_FALSE(hidden(460.0f, 256.0f));
        CHECK_FALSE(hidden(466.0f, 256.0f));
        CHECK_FALSE(hidden(467.75f, 256.0f));
        CHECK(hidden(468.0f, 256.0f));
        CHECK(hidden(507.75f, 256.0f));
        CHECK_FALSE(hidden(508.0f, 256.0f));
        CHECK_FALSE(hidden(513.0f, 256.0f));
    }
    SUBCASE("the inset applies at the top edge") {
        CHECK_FALSE(hidden(488.0f, 220.0f));
        CHECK_FALSE(hidden(488.0f, 229.75f));
        CHECK(hidden(488.0f, 230.0f));
    }
    SUBCASE("but not at the bottom edge") {
        CHECK(hidden(488.0f, 273.75f));
        CHECK_FALSE(hidden(488.0f, 274.0f));
        CHECK_FALSE(hidden(488.0f, 280.0f));
    }
    SUBCASE("an inset wider than the top hides nothing; no inset uses the whole top") {
        CHECK_FALSE(hidden(488.0f, 256.0f, 30.0f));
        CHECK(hidden(464.0f, 226.0f, 0.0f));
        CHECK_FALSE(hidden(512.0f, 226.0f, 0.0f));
    }
}

TEST_CASE("fade: alpha moves at a constant rate and stops at the target") {
    // seconds 0.2: 5 alpha units per second, 0.25 per 0.05 s.
    float alpha = 1.0f;
    alpha = lam::step_fade(alpha, 0.35f, 0.2f, 0.05f);
    CHECK(alpha == doctest::Approx(0.75f));
    alpha = lam::step_fade(alpha, 0.35f, 0.2f, 0.05f);
    CHECK(alpha == doctest::Approx(0.5f));
    alpha = lam::step_fade(alpha, 0.35f, 0.2f, 0.05f);
    CHECK(alpha == 0.35f);
    alpha = lam::step_fade(alpha, 0.35f, 0.2f, 10.0f);
    CHECK(alpha == 0.35f);
    alpha = lam::step_fade(alpha, 1.0f, 0.2f, 0.05f);
    CHECK(alpha == doctest::Approx(0.6f));
    alpha = lam::step_fade(alpha, 1.0f, 0.2f, 0.1f);
    CHECK(alpha == 1.0f);
    alpha = lam::step_fade(alpha, 1.0f, 0.2f, 10.0f);
    CHECK(alpha == 1.0f);
}

TEST_CASE("fade: edge cases") {
    SUBCASE("seconds of 0 jumps") {
        CHECK(lam::step_fade(1.0f, 0.35f, 0.0f, 0.016f) == 0.35f);
        CHECK(lam::step_fade(0.35f, 1.0f, -1.0f, 0.016f) == 1.0f);
    }
    SUBCASE("dt of zero, negative or NaN keeps alpha") {
        CHECK(lam::step_fade(0.7f, 0.35f, 0.2f, 0.0f) == 0.7f);
        CHECK(lam::step_fade(0.7f, 1.0f, 0.2f, -1.0f) == 0.7f);
        CHECK(lam::step_fade(0.7f, 0.35f, 0.2f, std::numeric_limits<float>::quiet_NaN()) == 0.7f);
    }
    SUBCASE("alpha and target outside [0, 1] are pulled back; NaN alpha heals to the target") {
        CHECK(lam::step_fade(2.0f, 1.0f, 0.2f, 0.016f) == 1.0f);
        CHECK(lam::step_fade(-1.0f, 0.0f, 0.2f, 0.016f) == 0.0f);
        CHECK(lam::step_fade(0.5f, 7.0f, 0.0f, 0.016f) == 1.0f);
        CHECK(lam::step_fade(std::numeric_limits<float>::quiet_NaN(), 0.35f, 0.2f, 0.016f) ==
              0.35f);
    }
    SUBCASE("a change of mind half way turns around from where it is") {
        float alpha = lam::step_fade(1.0f, 0.35f, 0.2f, 0.1f);
        CHECK(alpha == doctest::Approx(0.5f));
        alpha = lam::step_fade(alpha, 1.0f, 0.2f, 0.05f);
        CHECK(alpha == doctest::Approx(0.75f));
    }
}

TEST_CASE("fade: a canopy fades only while the player's feet are behind it, then comes back") {
    const float dt = 1.0f / 60.0f;
    float alpha = 1.0f, lowest = 1.0f;
    bool was_hidden = false;
    for (float x = 400.0f; x < 600.0f; x += 90.0f * dt) {
        const bool h = hidden(x, 256.0f);
        const float before = alpha;
        alpha = lam::step_fade(alpha, h ? kTree.faded_alpha : 1.0f, 0.2f, dt);
        CAPTURE(x);
        CHECK(alpha >= 0.35f);
        CHECK(alpha <= 1.0f);
        if (h) {
            CHECK(alpha <= before);
            was_hidden = true;
        } else {
            CHECK(alpha >= before);
            if (!was_hidden) CHECK(alpha == 1.0f);
        }
        if (x > 490.0f && x < 507.0f) CHECK(alpha == 0.35f);
        lowest = std::min(lowest, alpha);
    }
    CHECK(was_hidden);
    CHECK(lowest == 0.35f);
    CHECK(alpha == 1.0f);
    alpha = 1.0f;
    for (float x = 400.0f; x < 600.0f; x += 90.0f * dt) {
        alpha = lam::step_fade(alpha, hidden(x, 300.0f) ? 0.35f : 1.0f, 0.2f, dt);
        CHECK(alpha == 1.0f);
    }
}
