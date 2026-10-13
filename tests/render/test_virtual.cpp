// compute_virtual_layout for each ScaleMode, and the framebuffer to virtual mapping used by input.
#include <initializer_list>
#include <optional>

#include <lam/render/virtual_screen.h>
#include <lam/test/support.h>

namespace {

using lam::IVec2;
using lam::ScaleMode;
using lam::Vec2;
using lam::VirtualLayout;

VirtualLayout expand(int w, int h) {
    return lam::compute_virtual_layout({w, h}, {480, 270}, ScaleMode::PixelExpand);
}

} // namespace

TEST_CASE("virtual layout: a phone in landscape (2400x1080)") {
    const VirtualLayout l = expand(2400, 1080);
    CHECK(l.scale == 4);
    CHECK(l.virtual_size == IVec2{600, 270});
}

TEST_CASE("virtual layout: Retina Mac window, 960x540 points = 1920x1080 pixels") {
    const VirtualLayout l = expand(1920, 1080);
    CHECK(l.scale == 4);
    CHECK(l.virtual_size == IVec2{480, 270});
}

TEST_CASE("virtual layout: the same window on a 1x display") {
    const VirtualLayout l = expand(960, 540);
    CHECK(l.scale == 2);
    CHECK(l.virtual_size == IVec2{480, 270});
}

TEST_CASE("virtual layout: sizes that are not a multiple of the scale round the virtual size up") {
    const VirtualLayout l = expand(3420, 2224);
    CHECK(l.scale == 8);
    CHECK(l.virtual_size == IVec2{428, 278});
}

TEST_CASE("virtual layout: the scale rounds to nearest, not down") {
    CHECK(expand(1642, 942).scale == 3);
    CHECK(expand(1680, 945).scale == 4);
    CHECK(expand(1280, 720).scale == 3);
}

TEST_CASE("virtual layout: the virtual image always covers the framebuffer, by less than one "
          "virtual pixel") {
    for (int height = 1; height <= 2400; height += 7) {
        for (int width : {1, 333, 960, 1080, 1920, 2400, 3420}) {
            const VirtualLayout l = expand(width, height);
            REQUIRE(l.scale >= 1);
            const int cw = l.virtual_size.x * l.scale, ch = l.virtual_size.y * l.scale;
            CHECK(cw >= width);
            CHECK(cw - width < l.scale);
            CHECK(ch >= height);
            CHECK(ch - height < l.scale);
        }
    }
}

TEST_CASE("virtual layout: degenerate framebuffer sizes do not divide by zero") {
    for (ScaleMode m :
         {ScaleMode::PixelExpand, ScaleMode::PixelPerfect, ScaleMode::Fit, ScaleMode::Stretch}) {
        const VirtualLayout l = lam::compute_virtual_layout({0, 0}, {0, -5}, m);
        CHECK(l.scale >= 1);
        CHECK(l.virtual_size.x >= 1);
        CHECK(l.virtual_size.y >= 1);
        CHECK(l.framebuffer == IVec2{1, 1});
    }
    const VirtualLayout l = expand(0, 0);
    CHECK(l.scale == 1);
    CHECK(l.virtual_size == IVec2{1, 1});
}

TEST_CASE("virtual layout: framebuffer positions map to the virtual pixel under them") {
    const VirtualLayout l = expand(2400, 1080); // scale 4
    std::optional<Vec2> p = lam::framebuffer_to_virtual(l, {0.0f, 0.0f});
    REQUIRE(p);
    CHECK(p->x == 0.0f);
    CHECK(p->y == 0.0f);
    p = lam::framebuffer_to_virtual(l, {3.9f, 4.0f});
    REQUIRE(p);
    CHECK(static_cast<int>(p->x) == 0);
    CHECK(static_cast<int>(p->y) == 1);
    p = lam::framebuffer_to_virtual(l, {2399.0f, 1079.0f});
    REQUIRE(p);
    CHECK(static_cast<int>(p->x) == 599);
    CHECK(static_cast<int>(p->y) == 269);
    CHECK_FALSE(lam::framebuffer_to_virtual(l, {-1.0f, 10.0f}));
    CHECK_FALSE(lam::framebuffer_to_virtual(l, {2400.0f, 10.0f}));
}

TEST_CASE("virtual layout: pixel perfect keeps the size, scales by an integer and letterboxes") {
    const VirtualLayout l =
        lam::compute_virtual_layout({1280, 800}, {320, 180}, ScaleMode::PixelPerfect);
    CHECK(l.scale == 4);
    CHECK(l.virtual_size == IVec2{320, 180});
    CHECK(l.viewport.w == 1280);
    CHECK(l.viewport.h == 720);
    CHECK(l.viewport.y == 40);
    CHECK_FALSE(lam::framebuffer_to_virtual(l, {10.0f, 20.0f})); // in the letterbox
    const std::optional<Vec2> p = lam::framebuffer_to_virtual(l, {6.0f, 44.0f});
    REQUIRE(p);
    CHECK(*p == Vec2{1.5f, 1.0f});
    CHECK(lam::virtual_to_framebuffer(l, {1.5f, 1.0f}) == Vec2{6.0f, 44.0f});
    // A framebuffer smaller than the target keeps scale 1 (crops, never scales down).
    CHECK(lam::compute_virtual_layout({200, 100}, {320, 180}, ScaleMode::PixelPerfect).scale == 1);
}

TEST_CASE("virtual layout: fit scales fractionally; stretch fills") {
    const VirtualLayout fit = lam::compute_virtual_layout({1000, 1000}, {480, 270}, ScaleMode::Fit);
    CHECK(fit.virtual_size == IVec2{480, 270});
    CHECK(fit.viewport.w == 1000);
    CHECK(fit.viewport.h == 563);
    CHECK(fit.scale == 2);
    const VirtualLayout st =
        lam::compute_virtual_layout({1000, 1000}, {480, 270}, ScaleMode::Stretch);
    CHECK(st.viewport.w == 1000);
    CHECK(st.viewport.h == 1000);
    CHECK(st.scale_xy.x == doctest::Approx(1000.0f / 480.0f));
    CHECK(st.scale_xy.y == doctest::Approx(1000.0f / 270.0f));
}
