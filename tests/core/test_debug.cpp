// DebugDraw and DebugOverlay. Both have fixed capacity and do nothing when LAM_DEV is 0.
#include <doctest/doctest.h>

#include <string>

#include <lam/core/debug.h>
#include <lam/test/support.h>

using namespace lam;

TEST_CASE("debug draw: primitives up to capacity, then dropped and counted") {
    DebugDraw dd(3);
    dd.line({0, 0}, {10, 0}, kWhite);
    dd.rect(Rect{1, 2, 3, 4}, kBlack, true);
    dd.circle({5, 5}, 2, kWhite);
    dd.text({1, 1}, "over", kWhite);
#if LAM_DEV
    REQUIRE(dd.prims().size() == 3);
    CHECK(dd.dropped() == 1);
    CHECK(dd.prims()[0].kind == DebugPrimKind::Line);
    CHECK(dd.prims()[0].b == Vec2{10, 0});
    CHECK(dd.prims()[1].kind == DebugPrimKind::Rect);
    CHECK(dd.prims()[1].a == Vec2{1, 2}); // min
    CHECK(dd.prims()[1].b == Vec2{3, 4}); // size
    CHECK(dd.prims()[1].screen);
    CHECK(dd.prims()[1].color == kBlack);
    CHECK(dd.prims()[2].radius == 2.0f);
    CHECK_FALSE(dd.prims()[2].screen);
    dd.clear();
    CHECK(dd.prims().empty());
    CHECK(dd.dropped() == 0);
    dd.text({0, 0}, std::string(100, 'x'), kWhite);
    REQUIRE(dd.prims().size() == 1);
    CHECK(dd.prims()[0].kind == DebugPrimKind::Text);
    CHECK(dd.prims()[0].text.size() == 47); // FixedString<48>
#else
    CHECK(dd.prims().empty());
    CHECK(dd.dropped() == 0);
#endif
}

TEST_CASE("debug draw: pushing never allocates") {
    DebugDraw dd(64);
    CHECK_NO_ALLOC(for (int i = 0; i < 100; ++i) dd.line({0, 0}, {1, 1}, kWhite));
    CHECK_NO_ALLOC(dd.text({0, 0}, "label", kWhite));
#if LAM_DEV
    CHECK(dd.prims().size() == 64);
    CHECK(dd.dropped() == 37);
#endif
    CHECK_NO_ALLOC(dd.clear());
}

TEST_CASE("debug overlay: stats replace by label; lines form a ring of 32") {
    DebugOverlay o;
    CHECK_FALSE(o.visible);
    CHECK_FALSE(o.show_colliders);
    o.stat("fps", 60);
    o.stat("entities", 120);
    o.stat("fps", 59);
    for (int i = 0; i < 40; ++i)
        o.text("line " + std::to_string(i));
#if LAM_DEV
    REQUIRE(o.stats().size() == 2);
    CHECK(o.stats()[0].label == "fps");
    CHECK(o.stats()[0].value == 59.0);
    CHECK(o.stats()[1].value == 120.0);
    REQUIRE(o.lines().size() == 32);
    CHECK(o.lines()[0].view() == "line 8"); // the oldest lines fell out
    CHECK(o.lines()[31].view() == "line 39");
    // Labels must have static storage: one-character views into a literal.
    constexpr std::string_view kLabels = "abcdefghijklmnopqrstuvwxyz0123456789"
                                         "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*"; // 70 labels
    for (std::size_t i = 0; i < kLabels.size(); ++i)
        o.stat(kLabels.substr(i, 1), static_cast<double>(i));
    CHECK(o.stats().size() == 64); // capped
#else
    CHECK(o.stats().empty());
    CHECK(o.lines().empty());
#endif
    o.clear_frame();
    CHECK(o.stats().empty());
    CHECK(o.lines().empty());
}
