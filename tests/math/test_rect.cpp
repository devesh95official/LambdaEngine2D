// Rect and IRect. Rects are half-open: [x, x + w).
#include <doctest/doctest.h>

#include <limits>

#include <lam/math/rect.h>

using namespace lam;

static_assert(Rect{0, 0, 10, 10}.contains({0, 0}));
static_assert(!Rect{0, 0, 10, 10}.contains({10, 0}));
static_assert(intersection(Rect{0, 0, 10, 10}, Rect{5, 5, 10, 10}) == Rect{5, 5, 5, 5});

TEST_CASE("rect: contains is half-open") {
    const Rect r{0, 0, 10, 10};
    CHECK(r.contains({0, 0}));
    CHECK(r.contains({9.999f, 9.999f}));
    CHECK_FALSE(r.contains({10, 5}));
    CHECK_FALSE(r.contains({5, 10}));
    CHECK_FALSE(r.contains({-0.001f, 5}));
    const IRect ir{2, 3, 4, 5};
    CHECK(ir.contains({2, 3}));
    CHECK(ir.contains({5, 7}));
    CHECK_FALSE(ir.contains({6, 3}));
    CHECK_FALSE(ir.contains({2, 8}));
}

TEST_CASE("rect: touching edges do not overlap") {
    const Rect a{0, 0, 10, 10};
    CHECK_FALSE(a.overlaps({10, 0, 5, 5}));
    CHECK_FALSE(a.overlaps({0, 10, 5, 5}));
    CHECK_FALSE(a.overlaps({-5, 0, 5, 5}));
    CHECK(a.overlaps({9, 0, 5, 5}));
    CHECK(a.overlaps({2, 2, 1, 1})); // inside
    CHECK(Rect{2, 2, 1, 1}.overlaps(a));
    const IRect ia{0, 0, 4, 4};
    CHECK_FALSE(ia.overlaps({4, 0, 2, 2}));
    CHECK(ia.overlaps({3, 3, 2, 2}));
}

TEST_CASE("rect: empty covers zero, negative and NaN sizes") {
    CHECK(Rect{0, 0, 0, 5}.empty());
    CHECK(Rect{0, 0, 5, -1}.empty());
    CHECK(Rect{0, 0, std::numeric_limits<float>::quiet_NaN(), 5}.empty());
    CHECK_FALSE(Rect{0, 0, 1, 1}.empty());
    CHECK(IRect{0, 0, 0, 1}.empty());
    CHECK(IRect{0, 0, 3, -1}.empty());
    CHECK_FALSE(IRect{0, 0, 1, 1}.empty());
}

TEST_CASE("rect: constructors and accessors") {
    const Rect c = Rect::from_center({5, 5}, {2, 3});
    CHECK(c == Rect{3, 2, 4, 6});
    CHECK(c.center() == Vec2{5, 5});
    CHECK(c.min() == Vec2{3, 2});
    CHECK(c.max() == Vec2{7, 8});
    CHECK(c.size() == Vec2{4, 6});
    CHECK(Rect::from_min_max({1, 2}, {4, 6}) == Rect{1, 2, 3, 4});
    CHECK(Rect{1, 1, 2, 2}.expanded(1) == Rect{0, 0, 4, 4});
    CHECK(Rect{1, 1, 2, 2}.translated({3, -1}) == Rect{4, 0, 2, 2});
    CHECK(to_rect(IRect{1, 2, 3, 4}) == Rect{1, 2, 3, 4});
}

TEST_CASE("rect: intersection") {
    CHECK(intersection(Rect{0, 0, 10, 10}, Rect{5, 5, 10, 10}) == Rect{5, 5, 5, 5});
    CHECK(intersection(Rect{0, 0, 10, 10}, Rect{2, 3, 1, 1}) == Rect{2, 3, 1, 1});
    const Rect apart = intersection(Rect{0, 0, 10, 10}, Rect{20, 20, 5, 5});
    CHECK(apart.empty());
    const Rect touching = intersection(Rect{0, 0, 10, 10}, Rect{10, 0, 5, 5});
    CHECK(touching.empty());
    CHECK(intersection(IRect{0, 0, 4, 4}, IRect{2, 1, 4, 4}) == IRect{2, 1, 2, 3});
    CHECK(intersection(IRect{0, 0, 4, 4}, IRect{9, 9, 1, 1}).empty());
}
