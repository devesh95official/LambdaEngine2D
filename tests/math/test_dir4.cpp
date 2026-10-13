// Dir4: facing from the dominant axis, +y down. Ties and tiny vectors keep the current facing.
#include <doctest/doctest.h>

#include <limits>

#include <lam/math/dir4.h>

using namespace lam;

static_assert(dir4_of({3, 1}, Dir4::Up) == Dir4::Right);   // usable in constant expressions
static_assert(dir4_offset(Dir4::Down) == IVec2{0, 1});

TEST_CASE("dir4: the dominant axis decides; +y is down") {
    CHECK(dir4_of({1, 0}, Dir4::Up) == Dir4::Right);
    CHECK(dir4_of({-1, 0}, Dir4::Up) == Dir4::Left);
    CHECK(dir4_of({0, 1}, Dir4::Up) == Dir4::Down);
    CHECK(dir4_of({0, -1}, Dir4::Down) == Dir4::Up);
    CHECK(dir4_of({0.9f, -0.4f}, Dir4::Up) == Dir4::Right);
    CHECK(dir4_of({-0.2f, 0.7f}, Dir4::Up) == Dir4::Down);
}

TEST_CASE("dir4: ties, tiny and non-finite vectors keep the current facing") {
    for (Dir4 cur : {Dir4::Down, Dir4::Left, Dir4::Right, Dir4::Up}) {
        CHECK(dir4_of({1, 1}, cur) == cur);
        CHECK(dir4_of({-0.5f, 0.5f}, cur) == cur);
        CHECK(dir4_of({0, 0}, cur) == cur);
        CHECK(dir4_of({0.0005f, 0}, cur) == cur);
        CHECK(dir4_of({std::numeric_limits<float>::quiet_NaN(), 1}, cur) == cur);
    }
    CHECK(dir4_of({0.002f, 0}, Dir4::Up) == Dir4::Right); // just above the 1e-3 dead zone
}

TEST_CASE("dir4: offsets, vectors and opposites") {
    CHECK(dir4_offset(Dir4::Down) == IVec2{0, 1});
    CHECK(dir4_offset(Dir4::Left) == IVec2{-1, 0});
    CHECK(dir4_offset(Dir4::Right) == IVec2{1, 0});
    CHECK(dir4_offset(Dir4::Up) == IVec2{0, -1});
    CHECK(dir4_vector(Dir4::Left) == Vec2{-1, 0});
    CHECK(dir4_vector(Dir4::Up) == Vec2{0, -1});
    for (Dir4 d : {Dir4::Down, Dir4::Left, Dir4::Right, Dir4::Up}) {
        CHECK(dir4_opposite(dir4_opposite(d)) == d);
        CHECK(dir4_offset(dir4_opposite(d)) == -dir4_offset(d));
        CHECK(dir4_of(dir4_vector(d), dir4_opposite(d)) == d); // round trip
    }
    CHECK(static_cast<int>(Dir4::Down) == 0); // sheet row order: Down, Left, Right, Up
    CHECK(static_cast<int>(Dir4::Up) == 3);
}
