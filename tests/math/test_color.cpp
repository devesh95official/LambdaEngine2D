// Color: hex values, lerp, multiply and alpha.
#include <doctest/doctest.h>

#include <limits>

#include <lam/math/color.h>

using namespace lam;

static_assert(Color::hex(0x11223344) == Color{0x11, 0x22, 0x33, 0x44});
static_assert(Color::hex(0xFF8000FF).with_alpha(7) == Color{255, 128, 0, 7});

TEST_CASE("color: hex and constants") {
    CHECK(Color::hex(0xFFFFFFFF) == kWhite);
    CHECK(Color::hex(0x000000FF) == kBlack);
    CHECK(Color::hex(0x00000000) == kTransparent);
    CHECK(Color{} == kWhite);
}

TEST_CASE("color: lerp clamps t and rounds to nearest") {
    CHECK(lerp(kBlack, kWhite, 0.5f) == Color{128, 128, 128, 255});
    CHECK(lerp(kBlack, kWhite, 0.0f) == kBlack);
    CHECK(lerp(kBlack, kWhite, 1.0f) == kWhite);
    CHECK(lerp(kBlack, kWhite, 2.0f) == kWhite);
    CHECK(lerp(kBlack, kWhite, -1.0f) == kBlack);
    CHECK(lerp(kBlack, kWhite, std::numeric_limits<float>::quiet_NaN()) == kBlack);
    CHECK(lerp(Color{0, 0, 0, 0}, Color{10, 20, 30, 40}, 0.25f) == Color{3, 5, 8, 10});
    CHECK(lerp(Color{200, 100, 50, 255}, Color{0, 0, 0, 255}, 0.5f) == Color{100, 50, 25, 255});
}

TEST_CASE("color: multiply") {
    CHECK(multiply(Color{255, 128, 0, 255}, Color{128, 255, 255, 255}) == Color{128, 128, 0, 255});
    for (int v = 0; v < 256; ++v) {
        const Color c{static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(255 - v), 7, 200};
        REQUIRE(multiply(c, kWhite) == c);
        REQUIRE(multiply(kWhite, c) == c);
        REQUIRE(multiply(c, kTransparent) == kTransparent);
    }
}

TEST_CASE("color: scale_alpha and luma") {
    CHECK(scale_alpha(kWhite, 0.5f) == Color{255, 255, 255, 128});
    CHECK(scale_alpha(kWhite, 2.0f) == kWhite);
    CHECK(scale_alpha(kWhite, -1.0f) == Color{255, 255, 255, 0});
    CHECK(scale_alpha(Color{1, 2, 3, 100}, 0.0f) == Color{1, 2, 3, 0});
    CHECK(luma(kWhite) == doctest::Approx(1.0f));
    CHECK(luma(kBlack) == 0.0f);
    CHECK(luma(Color{0, 255, 0, 255}) > luma(Color{255, 0, 0, 255}));
    CHECK(luma(Color{255, 0, 0, 255}) > luma(Color{0, 0, 255, 255}));
}
