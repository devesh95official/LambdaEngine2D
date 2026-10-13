// move_box in top-down mode. A box of size (w, h) standing on feet (x, y) is
// Rect{x - w/2, y - h, w, h}.
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>

#include <lam/physics/tile_world.h>
#include <lam/test/support.h>

#include "physics_test_util.h"

using lam::BoxMove;
using lam::MoveBoxOptions;
using lam::Rect;
using lam::TileShape;
using lam::TileWorld;
using lam::Vec2;
using lam::test::feet_box;
using lam::test::grid_from;
using lam::test::open_grid;

namespace {

// The typical foot box: 10 wide, 6 tall.
constexpr float kW = 10.0f;
constexpr float kH = 6.0f;

Rect box_at(float feet_x, float feet_y, float w = kW, float h = kH) { return feet_box(feet_x, feet_y, w, h); }

BoxMove move(const TileWorld &g, Rect box, float dx, float dy, float assist) {
    MoveBoxOptions o;
    o.corner_assist = assist;
    return lam::move_box(g, box, Vec2{dx, dy}, o);
}

bool blocked(const TileWorld &g, Rect box) { return lam::box_blocked(g, box); }

void set_solid(TileWorld &g, int x, int y) { g.set({x, y}, TileShape::Solid, 0); }

struct Direction {
    float x;
    float y;
};
constexpr Direction kEight[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};

struct Lcg {
    std::uint32_t state;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    }
};

} // namespace

TEST_CASE("tile sweep: touching a solid tile's edge is not overlapping it") {
    const TileWorld g = grid_from({".....", "..#..", "....."});
    // The solid tile covers x 32..48, y 16..32.
    CHECK_FALSE(blocked(g, box_at(27.0f, 30.0f)));                // right edge exactly at 32
    CHECK(blocked(g, box_at(27.5f, 30.0f)));
    CHECK_FALSE(blocked(g, box_at(53.0f, 30.0f)));                // left edge exactly at 48
    CHECK(blocked(g, box_at(52.5f, 30.0f)));
    CHECK_FALSE(blocked(g, box_at(40.0f, 16.0f)));                // bottom edge exactly at 16
    CHECK(blocked(g, box_at(40.0f, 16.5f)));
    CHECK_FALSE(blocked(g, box_at(40.0f, 38.0f)));                // top edge exactly at 32
    CHECK(blocked(g, box_at(40.0f, 37.5f)));
}

TEST_CASE("tile sweep: everything outside the map is solid") {
    const TileWorld g = open_grid(4, 3);                         // 64 x 48 pixels
    CHECK(g.at({-1, 0}) == TileShape::Solid);
    CHECK(g.at({0, -1}) == TileShape::Solid);
    CHECK(g.at({4, 0}) == TileShape::Solid);
    CHECK(g.at({0, 3}) == TileShape::Solid);
    CHECK_FALSE(blocked(g, box_at(5.0f, 6.0f)));                  // flush in the top-left corner
    CHECK_FALSE(blocked(g, box_at(59.0f, 48.0f)));                // flush in the bottom-right corner
    CHECK(blocked(g, box_at(4.5f, 6.0f)));
    CHECK(blocked(g, box_at(5.0f, 5.5f)));
    CHECK(blocked(g, box_at(59.5f, 48.0f)));
    CHECK(blocked(g, box_at(59.0f, 48.5f)));
    CHECK(blocked(g, box_at(-500.0f, 20.0f)));
    CHECK(blocked(g, box_at(1.0e30f, 20.0f)));

    // Walking off each edge stops exactly at it.
    const Rect start = box_at(32.0f, 24.0f);
    BoxMove m = move(g, start, -1000.0f, 0.0f, 0.0f);
    CHECK(m.box.x == 0.0f);
    CHECK(m.hit_x);
    m = move(g, start, 1000.0f, 0.0f, 0.0f);
    CHECK(m.box.x == 54.0f);                                       // right edge at 64
    m = move(g, start, 0.0f, -1000.0f, 0.0f);
    CHECK(m.box.y == 0.0f);
    CHECK(m.hit_y);
    m = move(g, start, 0.0f, 1000.0f, 0.0f);
    CHECK(m.box.y == 42.0f);                                       // bottom edge at 48
}

TEST_CASE("tile sweep: a free move goes the whole way and reports no hit") {
    const TileWorld g = open_grid(10, 10);
    const Rect start = box_at(40.0f, 40.0f);
    const BoxMove m = move(g, start, 13.25f, -7.5f, 5.0f);
    CHECK(m.box.x == start.x + 13.25f);
    CHECK(m.box.y == start.y - 7.5f);
    CHECK_FALSE(m.hit_x);
    CHECK_FALSE(m.hit_y);
    const BoxMove still = move(g, start, 0.0f, 0.0f, 5.0f);
    CHECK(still.box == start);
    CHECK_FALSE(still.hit_x);
    CHECK_FALSE(still.hit_y);
}

TEST_CASE("tile sweep: a wall met head-on stops the box exactly on the tile boundary") {
    const TileWorld g = grid_from({"#######", "#.....#", "#.....#", "#.....#", "#######"});
    // The room is x 16..96, y 16..64.
    const Rect start = box_at(50.0f, 40.0f);
    BoxMove m = move(g, start, 60.0f, 0.0f, 0.0f);
    CHECK(m.box.x + m.box.w == 96.0f);
    CHECK(m.box.y == start.y);
    CHECK(m.hit_x);
    CHECK_FALSE(m.hit_y);
    m = move(g, start, -60.0f, 0.0f, 0.0f);
    CHECK(m.box.x == 16.0f);
    CHECK(m.hit_x);
    m = move(g, start, 0.0f, 60.0f, 0.0f);
    CHECK(m.box.y + m.box.h == 64.0f);
    CHECK(m.hit_y);
    CHECK_FALSE(m.hit_x);
    m = move(g, start, 0.0f, -60.0f, 0.0f);
    CHECK(m.box.y == 16.0f);
    CHECK(m.hit_y);
    // Already flush: pushing on moves nothing, and no gap ever creeps in.
    const Rect flush = box_at(91.0f, 40.0f);
    for (int i = 0; i < 100; ++i) {
        m = move(g, flush, 1.5f, 0.0f, 0.0f);
        CHECK(m.box.x == flush.x);
        CHECK(m.hit_x);
    }
}

TEST_CASE("tile sweep: one large step ends where many small steps end") {
    const TileWorld g = grid_from({"#########", "#.......#", "#.......#", "#.......#", "#########"});
    for (const Direction d : kEight) {
        const Rect start = box_at(70.3f, 41.7f);
        const BoxMove big = move(g, start, d.x * 500.0f, d.y * 500.0f, 0.0f);
        Rect box = start;
        for (int i = 0; i < 2000; ++i) box = move(g, box, d.x * 0.37f, d.y * 0.37f, 0.0f).box;
        CAPTURE(d.x);
        CAPTURE(d.y);
        // An axis that was blocked ends on the same boundary, to the bit.
        if (d.x != 0.0f) CHECK(box.x == big.box.x);
        if (d.y != 0.0f) CHECK(box.y == big.box.y);
    }
}

TEST_CASE("tile sweep: no tunnelling at any speed, in eight directions") {
    // A room one tile of wall thick, with free ground beyond: a box that skipped the wall would
    // come to rest out there, unblocked, and this test would see it.
    TileWorld g = open_grid(40, 40);
    for (int i = 15; i <= 24; ++i) {
        set_solid(g, i, 15);
        set_solid(g, i, 24);
        set_solid(g, 15, i);
        set_solid(g, 24, i);
    }
    // Inside: x 256..384, y 256..384.
    const float starts[][2] = {{320.0f, 320.0f}, {261.0f, 262.0f}, {379.0f, 384.0f}, {300.3f, 371.9f},
                               {377.77f, 263.21f}};
    const float distances[] = {1.0f,   3.0f,   7.9f,   15.0f,   16.0f,   17.0f,    31.0f,  64.0f,  127.5f, 128.0f,
                               129.0f, 144.0f, 145.0f, 160.0f, 500.0f, 999.0f, 4096.0f, 10000.0f, 1.0e9f, 3.0e38f};
    for (const auto &s : starts) {
        const Rect start = box_at(s[0], s[1]);
        REQUIRE_FALSE(blocked(g, start));
        for (const Direction d : kEight) {
            for (const float dist : distances) {
                for (const float assist : {0.0f, 5.0f}) {
                    const BoxMove m = move(g, start, d.x * dist, d.y * dist, assist);
                    CAPTURE(s[0]);
                    CAPTURE(s[1]);
                    CAPTURE(d.x);
                    CAPTURE(d.y);
                    CAPTURE(dist);
                    CHECK_FALSE(blocked(g, m.box));
                    CHECK(m.box.x >= 256.0f);
                    CHECK(m.box.x + m.box.w <= 384.0f);
                    CHECK(m.box.y >= 256.0f);
                    CHECK(m.box.y + m.box.h <= 384.0f);
                }
            }
        }
    }
}

TEST_CASE("tile sweep: a fast shallow-angle move cannot slip through a wall or past its end") {
    const TileWorld g = grid_from({"..........", "..........", "..........", "##########", ".........."});
    // The wall's top is y = 48. Coming down at shallow and steep angles, any speed.
    const Rect start = box_at(80.0f, 47.0f);
    for (const float dx : {-900.0f, -40.0f, -0.01f, 0.01f, 3.0f, 77.0f, 900.0f}) {
        for (const float dy : {0.001f, 0.5f, 4.0f, 16.0f, 33.0f, 400.0f}) {
            const BoxMove m = move(g, start, dx, dy, 0.0f);
            CAPTURE(dx);
            CAPTURE(dy);
            CHECK(m.box.y + m.box.h <= 48.0f);
            CHECK_FALSE(blocked(g, m.box));
            if (dy >= 1.0f) {
                CHECK(m.box.y + m.box.h == 48.0f);
                CHECK(m.hit_y);
            }
        }
    }
}

TEST_CASE("tile sweep: sliding along a wall is exactly free movement on the other axis") {
    // The box is pushed diagonally into each of the four walls in turn. Tick by tick, its
    // coordinate along the wall must equal the one a box in an empty field has, to the bit.
    const TileWorld room = grid_from({"####################", "#..................#", "#..................#",
                                      "#..................#", "#..................#", "####################"});
    const TileWorld field = open_grid(20, 6);
    // Inside: x 16..304, y 16..80.
    struct Case {
        float x, y, dx, dy;
        bool along_x;
    };
    const Case cases[] = {
        {30.0f, 80.0f, 1.37f, 2.0f, true},    {290.0f, 80.0f, -1.37f, 0.9f, true},
        {30.0f, 22.0f, 1.37f, -2.0f, true},   {290.0f, 22.0f, -0.73f, -5.0f, true},
        {21.0f, 30.0f, -2.0f, 0.31f, false},  {21.0f, 70.0f, -0.4f, -0.31f, false},
        {299.0f, 30.0f, 2.0f, 0.31f, false},  {299.0f, 70.0f, 3.3f, -0.31f, false},
    };
    for (const Case &c : cases) {
        for (const float assist : {0.0f, 5.0f}) {
            const Rect start = box_at(c.x, c.y);
            REQUIRE_FALSE(blocked(room, start));
            Rect box = start;
            Rect free = start;
            for (int tick = 0; tick < 120; ++tick) {
                const BoxMove slid = move(room, box, c.dx, c.dy, assist);
                const BoxMove open = move(field, free, c.dx, c.dy, assist);
                CAPTURE(tick);
                CAPTURE(c.dx);
                CAPTURE(c.dy);
                if (c.along_x) {
                    REQUIRE(slid.box.x == open.box.x);
                    REQUIRE(slid.box.y == start.y);
                    REQUIRE(slid.hit_y);
                    REQUIRE_FALSE(slid.hit_x);
                } else {
                    REQUIRE(slid.box.y == open.box.y);
                    REQUIRE(slid.box.x == start.x);
                    REQUIRE(slid.hit_x);
                    REQUIRE_FALSE(slid.hit_y);
                }
                box = slid.box;
                free = open.box;
                // Keep the free box in step on the wall's axis, so only the sliding axis compares.
                if (c.along_x) free.y = start.y;
                else free.x = start.x;
            }
        }
    }
}

TEST_CASE("tile sweep: walking along a wall never stops or jitters on tile seams") {
    const TileWorld room = grid_from({"########################", "#......................#",
                                      "#......................#", "#......................#",
                                      "########################"});
    // Inside: x 16..368, y 16..64.
    for (int offset = 0; offset < 64; ++offset) {
        const float sub = static_cast<float>(offset) / 64.0f;
        for (const float speed : {0.25f, 1.0f, 1.5f, 1.37f, 2.7f, 15.9f, 16.0f, 16.1f}) {
            for (const float wall_y : {64.0f, 22.0f}) {
                for (const float sign : {1.0f, -1.0f}) {
                    Rect box = box_at((sign > 0.0f ? 22.0f : 362.0f) + sign * sub, wall_y);
                    int ticks = 0;
                    while (sign > 0.0f ? box.x + speed < 358.0f : box.x - speed > 16.0f) {
                        const BoxMove m = move(room, box, sign * speed, 0.0f, 5.0f);
                        REQUIRE(m.box.x == box.x + sign * speed);      // full progress, every tick
                        REQUIRE(m.box.y == box.y);                      // and not a hair sideways
                        REQUIRE_FALSE(m.hit_x);
                        box = m.box;
                        ++ticks;
                    }
                    REQUIRE(ticks > 10);
                }
            }
        }
    }
    const TileWorld tower = grid_from({"#####", "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", "#...#",
                                       "#...#", "#...#", "#...#", "#####"});
    // Inside: x 16..64, y 16..176.
    for (int offset = 0; offset < 64; ++offset) {
        const float sub = static_cast<float>(offset) / 64.0f;
        for (const float speed : {0.25f, 1.0f, 1.37f, 2.7f, 16.0f}) {
            for (const float wall_x : {21.0f, 59.0f}) {
                for (const float sign : {1.0f, -1.0f}) {
                    Rect box = box_at(wall_x, (sign > 0.0f ? 24.0f : 174.0f) + sign * sub);
                    int ticks = 0;
                    while (sign > 0.0f ? box.y + box.h + speed < 176.0f : box.y - speed > 16.0f) {
                        const BoxMove m = move(tower, box, 0.0f, sign * speed, 5.0f);
                        REQUIRE(m.box.y == box.y + sign * speed);
                        REQUIRE(m.box.x == box.x);
                        REQUIRE_FALSE(m.hit_y);
                        box = m.box;
                        ++ticks;
                    }
                    REQUIRE(ticks > 5);
                }
            }
        }
    }
}

TEST_CASE("tile sweep: a one-tile corridor is walked from end to end, both axes") {
    const TileWorld g = grid_from({"############", "#..........#", "####.#######", "####.#######", "####.#######",
                                   "#..........#", "############"});
    // Horizontal corridor: row 1 (y 16..32), x 16..176. The 6-tall box fits at any feet y in 22..32.
    for (int step = 0; step <= 40; ++step) {
        const float y = 22.0f + static_cast<float>(step) * 0.25f;
        for (const float push : {0.0f, -3.0f}) {
            Rect box = box_at(21.0f, y);
            for (int tick = 0; tick < 400 && box.x + box.w < 176.0f; ++tick) {
                const BoxMove m = move(g, box, 1.7f, push, 5.0f);
                if (m.box.x + m.box.w < 176.0f) REQUIRE(m.box.x == box.x + 1.7f);
                box = m.box;
                REQUIRE_FALSE(blocked(g, box));
            }
            CHECK(box.x + box.w == 176.0f);                         // reached the far wall
        }
    }
    // Vertical corridor: column 4 (x 64..80), rows 2..4 (y 32..80). The 10-wide box fits at feet x
    // 69..75; inside the shaft every tick must make full progress, across the seams.
    for (int step = 0; step <= 24; ++step) {
        const float x = 69.0f + static_cast<float>(step) * 0.25f;
        for (const float push : {0.0f, -3.0f, 3.0f}) {
            CAPTURE(x);
            CAPTURE(push);
            Rect box = box_at(x, 40.0f);                            // box y 34..40: just inside the top
            int ticks = 0;
            while (box.y + box.h + 1.7f <= 80.0f) {
                const BoxMove m = move(g, box, push, 1.7f, 5.0f);
                REQUIRE(m.box.y == box.y + 1.7f);
                REQUIRE_FALSE(m.hit_y);
                REQUIRE(m.box.x >= 64.0f);
                REQUIRE(m.box.x + m.box.w <= 80.0f);
                box = m.box;
                ++ticks;
            }
            CHECK(ticks == 23);
            // And up again from the bottom of the shaft (box y 74..80).
            box = box_at(x, 80.0f);
            ticks = 0;
            while (box.y - 1.7f >= 32.0f) {
                const BoxMove m = move(g, box, push, -1.7f, 5.0f);
                REQUIRE(m.box.y == box.y - 1.7f);
                REQUIRE_FALSE(m.hit_y);
                box = m.box;
                ++ticks;
            }
            CHECK(ticks == 24);
        }
    }
    // Straight down the shaft from the top corridor's floor, and out at the bottom.
    Rect box = box_at(72.0f, 32.0f);
    for (int tick = 0; tick < 100; ++tick) box = move(g, box, 0.0f, 1.7f, 5.0f).box;
    CHECK(box.y + box.h == 96.0f);
}

TEST_CASE("tile sweep: a doorway one tile wide is entered while pressing diagonally") {
    const TileWorld g = grid_from({"#########", "#.......#", "####.####", "#.......#", "#########"});
    // Running right along the top room's floor holding down-right: at the doorway (x 64..80) the
    // box must drop in instead of skating over it.
    Rect box = box_at(21.0f, 32.0f);
    for (int tick = 0; tick < 300; ++tick) {
        box = move(g, box, 1.0f, 1.0f, 5.0f).box;
        REQUIRE_FALSE(blocked(g, box));
    }
    CHECK(box.y + box.h == 64.0f);                                  // on the lower room's floor
}

TEST_CASE("tile sweep: an inner corner stops both axes; pushing on changes nothing") {
    const TileWorld g = grid_from({"######", "#....#", "#....#", "######"});
    // Inside: x 16..80, y 16..48.
    for (const Direction d : {Direction{1, 1}, Direction{-1, 1}, Direction{1, -1}, Direction{-1, -1}}) {
        Rect box = box_at(48.0f, 35.0f);
        for (int tick = 0; tick < 200; ++tick) box = move(g, box, d.x * 1.3f, d.y * 1.3f, 5.0f).box;
        CHECK(box.x == (d.x > 0.0f ? 70.0f : 16.0f));
        CHECK(box.y == (d.y > 0.0f ? 42.0f : 16.0f));
        const BoxMove stuck = move(g, box, d.x * 1.3f, d.y * 1.3f, 5.0f);
        CHECK(stuck.box == box);
        CHECK(stuck.hit_x);
        CHECK(stuck.hit_y);
    }
}

TEST_CASE("tile sweep: an outer corner is rounded without sticking") {
    const TileWorld g = grid_from({".........", ".........", "....#....", ".........", "........."});
    // The block is x 64..80, y 32..48. Run at it diagonally from each side.
    struct Run {
        float x, y, dx, dy;
    };
    const Run runs[] = {{30.0f, 20.0f, 1.0f, 1.0f},   {110.0f, 20.0f, -1.0f, 1.0f}, {30.0f, 70.0f, 1.0f, -1.0f},
                        {110.0f, 70.0f, -1.0f, -1.0f}, {40.0f, 30.0f, 1.0f, 0.3f},  {70.0f, 12.0f, 0.3f, 1.0f}};
    for (const Run &run : runs) {
        Rect box = box_at(run.x, run.y);
        int still = 0;
        for (int tick = 0; tick < 60; ++tick) {
            const BoxMove m = move(g, box, run.dx, run.dy, 5.0f);
            if (m.box == box) ++still;
            box = m.box;
            REQUIRE_FALSE(blocked(g, box));
        }
        CAPTURE(run.x);
        CAPTURE(run.y);
        CHECK(still == 0);                                          // some progress on every tick
    }
}

TEST_CASE("tile sweep: corner assist steers a straight move round a corner it only clips") {
    const TileWorld g = grid_from({".........", ".........", "....#....", ".........", "........."});
    // The block is x 64..80, y 32..48. The box walks right, flush against its west face, clipping
    // its top corner by 3 px: box y 29..35.
    BoxMove m = move(g, box_at(59.0f, 35.0f), 1.0f, 0.0f, 5.0f);
    CHECK(m.box.x == 54.0f);
    CHECK(m.box.y == 28.0f);                                        // up by the forward distance lost
    CHECK_FALSE(m.hit_x);
    CHECK_FALSE(m.hit_y);
    // A step larger than the shift needed lands exactly clear of the corner.
    m = move(g, box_at(59.0f, 35.0f), 8.0f, 0.0f, 5.0f);
    CHECK(m.box.y + m.box.h == 32.0f);
    CHECK(m.box.x == 54.0f);
    // From there the way is open.
    m = move(g, m.box, 8.0f, 0.0f, 5.0f);
    CHECK(m.box.x == 62.0f);
    CHECK(m.box.y + m.box.h == 32.0f);
    // Clipping the bottom corner by 3 px (box y 45..51): steered down instead.
    m = move(g, box_at(59.0f, 51.0f), 1.0f, 0.0f, 5.0f);
    CHECK(m.box.y + m.box.h == 52.0f);
    CHECK_FALSE(m.hit_x);
    // The same from the east, the north and the south.
    m = move(g, box_at(85.0f, 35.0f), -1.0f, 0.0f, 5.0f);
    CHECK(m.box.x == 80.0f);
    CHECK(m.box.y + m.box.h == 34.0f);
    m = move(g, box_at(62.0f, 32.0f), 0.0f, 1.0f, 5.0f);            // box x 57..67: clips 3 px
    CHECK(m.box.y + m.box.h == 32.0f);
    CHECK(m.box.x == 56.0f);
    CHECK_FALSE(m.hit_y);
    m = move(g, box_at(82.0f, 54.0f), 0.0f, -1.0f, 5.0f);           // box x 77..87
    CHECK(m.box.y + m.box.h == 54.0f);
    CHECK(m.box.x == 78.0f);
    CHECK_FALSE(m.hit_y);
}

TEST_CASE("tile sweep: corner assist has limits") {
    const TileWorld g = grid_from({".........", ".........", "....#....", ".........", "........."});
    // Clipping by 6 px (box y 32..38 needs 6 up): beyond a reach of 5, so it is a wall.
    BoxMove m = move(g, box_at(59.0f, 38.0f), 1.0f, 0.0f, 5.0f);
    CHECK(m.box == box_at(59.0f, 38.0f));
    CHECK(m.hit_x);
    // Exactly at the reach it still helps.
    m = move(g, box_at(59.0f, 37.0f), 1.0f, 0.0f, 5.0f);
    CHECK(m.box.y + m.box.h == 36.0f);
    CHECK_FALSE(m.hit_x);
    // Switched off: a clipped corner is a wall.
    m = move(g, box_at(59.0f, 35.0f), 1.0f, 0.0f, 0.0f);
    CHECK(m.box.y + m.box.h == 35.0f);
    CHECK(m.hit_x);
    // Only for moves along one axis: with any sideways input the player is steering already.
    m = move(g, box_at(59.0f, 35.0f), 1.0f, 0.5f, 5.0f);
    CHECK(m.box.x == 54.0f);
    CHECK(m.box.y + m.box.h == 35.5f);
    CHECK(m.hit_x);
    // Only when blocked from the start: a box that still has room walks up to the block.
    m = move(g, box_at(57.0f, 35.0f), 4.0f, 0.0f, 5.0f);
    CHECK(m.box.x == 54.0f);
    CHECK(m.box.y + m.box.h == 35.0f);
    CHECK(m.hit_x);
}

TEST_CASE("tile sweep: of two possible corner shifts the smaller wins, a tie goes negative") {
    const TileWorld g = grid_from({".......", ".......", "...#...", ".......", "......."});
    // The post is x 48..64, y 32..48. A 4 x 4 box walking up from below (feet y = 52).
    constexpr float s = 4.0f;
    // Box x 49..53: 3 to clear on the left (to x = 44..48), 13 on the right.
    BoxMove m = move(g, box_at(51.0f, 52.0f, s, s), 0.0f, -1.0f, 15.0f);
    CHECK(m.box.x == 48.0f);
    // Box x 59..63: 13 on the left, 3 on the right.
    m = move(g, box_at(61.0f, 52.0f, s, s), 0.0f, -1.0f, 15.0f);
    CHECK(m.box.x == 60.0f);
    // Dead centre (box x 54..58): 10 either way; the tie goes toward negative x.
    m = move(g, box_at(56.0f, 52.0f, s, s), 0.0f, -1.0f, 15.0f);
    CHECK(m.box.x == 53.0f);
    // The same on the other axis: walking right into the post's west face.
    m = move(g, box_at(46.0f, 42.0f, s, s), 1.0f, 0.0f, 15.0f);     // box y 38..42: centre
    CHECK(m.box.y + m.box.h == 41.0f);
    m = move(g, box_at(46.0f, 45.0f, s, s), 1.0f, 0.0f, 15.0f);     // box y 41..45: 3 down
    CHECK(m.box.y + m.box.h == 46.0f);
}

TEST_CASE("tile sweep: corner assist never moves the box into something solid") {
    const TileWorld g = grid_from({".........", "....#....", "....#....", ".........", "....#....", "........."});
    // Walking right against the block at row 2 (y 32..48), clipping its bottom by 2 px.
    BoxMove m = move(g, box_at(59.0f, 52.0f), 1.0f, 0.0f, 5.0f);
    CHECK(m.box.y + m.box.h == 53.0f);
    // With the gap filled there is nowhere to be steered to: it must stay put.
    TileWorld filled = g;
    set_solid(filled, 4, 3);
    m = move(filled, box_at(59.0f, 52.0f), 1.0f, 0.0f, 5.0f);
    CHECK(m.box == box_at(59.0f, 52.0f));
    CHECK(m.hit_x);
    // The way ahead would be open after the shift, but the shift itself is blocked.
    constexpr float s = 4.0f;
    const TileWorld open_below = grid_from({".....", "...#.", ".....", "....."});
    m = move(open_below, box_at(46.0f, 31.0f, s, s), 1.0f, 0.0f, 5.0f);
    CHECK(m.box.y + m.box.h == 32.0f);                              // control: steered down
    CHECK_FALSE(m.hit_x);
    const TileWorld pinned = grid_from({".....", "...#.", "..#..", "....."});
    m = move(pinned, box_at(46.0f, 31.0f, s, s), 1.0f, 0.0f, 5.0f);
    CHECK(m.box == box_at(46.0f, 31.0f, s, s));
    CHECK(m.hit_x);
}

TEST_CASE("tile sweep: a box that starts inside something solid moves only where its overlap does not grow") {
    // It used to stay put for good, whatever the direction.
    const TileWorld g = grid_from({"....", ".#..", "...."});
    const Rect start = box_at(24.0f, 28.0f);                        // wholly inside the block
    const BoxMove m = move(g, start, 30.0f, -12.0f, 5.0f);
    CHECK(m.box == Rect{49.0f, 10.0f, kW, kH});                     // out on x, then free on y
    CHECK_FALSE(m.hit_x);
    CHECK_FALSE(m.hit_y);
    const Rect edge = box_at(24.0f, 36.0f);                         // 2 px into the block's bottom
    const BoxMove deeper = move(g, edge, 0.0f, -3.0f, 5.0f);
    CHECK(deeper.box == edge);
    CHECK(deeper.hit_y);
    const BoxMove out = move(g, edge, 0.0f, 3.0f, 5.0f);
    CHECK(out.box.y == edge.y + 3.0f);
    CHECK_FALSE(out.hit_y);
}

TEST_CASE("tile sweep: non-finite input leaves the box where it is") {
    const TileWorld g = open_grid(8, 8);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const Rect start = box_at(40.0f, 40.0f);
    for (const float bad : {nan, inf, -inf}) {
        CHECK(move(g, start, bad, 1.0f, 5.0f).box == start);
        CHECK(move(g, start, 1.0f, bad, 5.0f).box == start);
        Rect broken = start;
        broken.x = bad;
        const BoxMove m = move(g, broken, 1.0f, 1.0f, 5.0f);
        CHECK_FALSE(std::isfinite(m.box.x));                        // returned as given, no walk
    }
    // A NaN corner assist counts as switched off.
    CHECK(move(g, start, 2.0f, 0.0f, nan).box.x == start.x + 2.0f);
}

TEST_CASE("tile sweep: the same inputs give the same result") {
    const TileWorld g = grid_from({"########", "#..#...#", "#......#", "#.#..#.#", "########"});
    Rect a = box_at(24.0f, 30.0f);
    Rect b = a;
    Lcg la{7u}, lb{7u};
    for (int tick = 0; tick < 2000; ++tick) {
        const float adx = la.next() * 8.0f - 4.0f, ady = la.next() * 8.0f - 4.0f;
        const float bdx = lb.next() * 8.0f - 4.0f, bdy = lb.next() * 8.0f - 4.0f;
        const BoxMove ma = move(g, a, adx, ady, 5.0f);
        const BoxMove mb = move(g, b, bdx, bdy, 5.0f);
        REQUIRE(ma.box == mb.box);
        REQUIRE(ma.hit_x == mb.hit_x);
        REQUIRE(ma.hit_y == mb.hit_y);
        a = ma.box;
        b = mb.box;
    }
}

TEST_CASE("tile sweep: random walks through a maze never end inside a wall") {
    const TileWorld g = grid_from({"################", "#....#.....#...#", "#.##.#.###.#.#.#", "#.#........#.#.#",
                                   "#.#.####.###.#.#", "#...#..........#", "###.#.####.###.#", "#.....#....#...#",
                                   "################"});
    // Awkward sizes on purpose: halves that are not whole numbers exercise boundary rounding.
    const float sizes[][2] = {{kW, kH}, {10.3f, 6.7f}, {15.9f, 15.9f}, {1.0f, 1.0f}, {7.77f, 3.33f}};
    for (const auto &sz : sizes) {
        for (std::uint32_t seed = 1; seed <= 4; ++seed) {
            Lcg lcg{seed * 2654435761u};
            Rect box = box_at(24.0f, 24.0f + sz[1] * 0.5f, sz[0], sz[1]);
            REQUIRE_FALSE(blocked(g, box));
            float dx = 0.0f, dy = 0.0f;
            for (int tick = 0; tick < 6000; ++tick) {
                if (tick % 23 == 0) {
                    const float scale = lcg.next() < 0.1f ? 300.0f : 3.0f;
                    dx = (lcg.next() * 2.0f - 1.0f) * scale;
                    dy = (lcg.next() * 2.0f - 1.0f) * scale;
                    if (lcg.next() < 0.3f) dx = 0.0f;
                    else if (lcg.next() < 0.3f) dy = 0.0f;
                }
                const BoxMove m = move(g, box, dx, dy, 5.0f);
                // Never further than asked (assist adds sideways travel up to the forward distance).
                const float reach = std::fabs(dx) + std::fabs(dy) + 0.001f;
                REQUIRE(std::fabs(m.box.x - box.x) <= reach);
                REQUIRE(std::fabs(m.box.y - box.y) <= reach);
                box = m.box;
                REQUIRE_FALSE(blocked(g, box));
            }
        }
    }
}

TEST_CASE("tile sweep: a box whose half width is not a whole number still stops flush") {
    const TileWorld g = grid_from({"######", "#....#", "#....#", "######"});
    const Rect start = box_at(48.0f, 36.0f, 10.3f, 6.7f);
    for (const Direction d : kEight) {
        const BoxMove m = move(g, start, d.x * 200.0f, d.y * 200.0f, 0.0f);
        CHECK_FALSE(blocked(g, m.box));
        if (d.x > 0.0f) CHECK(m.box.x + m.box.w == doctest::Approx(80.0f).epsilon(0.00001));
        if (d.x < 0.0f) CHECK(m.box.x == doctest::Approx(16.0f).epsilon(0.00001));
        if (d.y > 0.0f) CHECK(m.box.y + m.box.h == doctest::Approx(48.0f).epsilon(0.00001));
        if (d.y < 0.0f) CHECK(m.box.y == doctest::Approx(16.0f).epsilon(0.00001));
        // Pushing again changes nothing at all.
        const BoxMove again = move(g, m.box, d.x * 200.0f, d.y * 200.0f, 0.0f);
        CHECK(again.box == m.box);
    }
}

TEST_CASE("tile sweep: a rectangle overlaps exactly the tiles it covers") {
    // Only the two tiles under x 16..48, y 16..32 block it.
    TileWorld g = open_grid(6, 6);
    g.set({1, 1}, TileShape::Solid, 0);
    g.set({2, 1}, TileShape::Solid, 0);
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 6; ++x) {
            const Rect tile = g.tile_rect({x, y});
            const bool expected = y == 1 && (x == 1 || x == 2);
            CHECK(blocked(g, tile) == expected);
        }
    }
    CHECK(blocked(g, Rect{16.0f, 16.0f, 32.0f, 16.0f}));
    CHECK_FALSE(blocked(g, Rect{0.0f, 0.0f, 16.0f, 16.0f}));     // touches (1,1) only at a corner
}

TEST_CASE("tile sweep: a box only touching an entity solid is not blocked by it") {
    const TileWorld g = open_grid(8, 8);
    const Rect solid{20.0f, 10.0f, 10.0f, 10.0f};
    const Rect solids[] = {solid};
    CHECK(lam::box_blocked(g, feet_box(20.0f, 20.0f, kW, kH), solids));
    CHECK_FALSE(lam::box_blocked(g, feet_box(15.0f, 20.0f, kW, kH), solids));   // right edge at 20
    CHECK_FALSE(lam::box_blocked(g, feet_box(20.0f, 10.0f, kW, kH), solids));   // bottom edge at 10
}

TEST_CASE("tile sweep: nearest_free_spot moves a box out of a wall to the nearest free place") {
    const TileWorld g = grid_from({"#####", "#...#", "#.#.#", "#...#", "#####"});
    // Inside the middle wall tile (2,2): the nearest free place is beside it.
    const Rect inside = box_at(40.0f, 44.0f);
    const auto spot = lam::nearest_free_spot(g, inside, 3);
    REQUIRE(spot.has_value());
    const Rect placed{spot->x, spot->y, inside.w, inside.h};
    CHECK_FALSE(blocked(g, placed));
    CHECK(lam::distance(placed.center(), inside.center()) <= 16.0f);
    // A free place is returned as it is.
    const Rect free = box_at(24.0f, 28.0f);
    const auto same = lam::nearest_free_spot(g, free, 3);
    REQUIRE(same.has_value());
    CHECK(*same == free.min());
    // Nothing free within reach.
    const TileWorld solid = grid_from({"#####", "#####", "#####"});
    CHECK_FALSE(lam::nearest_free_spot(solid, box_at(40.0f, 30.0f), 1).has_value());
}

TEST_CASE("tile sweep: tile_at and tile_center agree, at any tile size and origin") {
    TileWorld g = open_grid(4, 4);
    const lam::IVec2 t = g.tile_at({37.0f, 5.0f});
    CHECK(t == lam::IVec2{2, 0});
    CHECK(g.tile_center(t) == Vec2{40.0f, 8.0f});
    g.resize(4, 4, 24, Vec2{-100.0f, 50.0f});
    CHECK(g.tile_at({-100.0f, 50.0f}) == lam::IVec2{0, 0});
    CHECK(g.tile_at({-77.0f, 73.9f}) == lam::IVec2{0, 0});
    CHECK(g.tile_at({-76.0f, 74.0f}) == lam::IVec2{1, 1});
    CHECK(g.tile_at({-101.0f, 49.0f}) == lam::IVec2{-1, -1});
    CHECK(g.tile_center({1, 2}) == Vec2{-64.0f, 110.0f});
}
