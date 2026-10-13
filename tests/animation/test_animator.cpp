// Animator playback in tick time. Calls detail::tick_animator directly, so no plugins are needed.
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <lam/animation/animator.h>
#include <lam/render/sprite.h>

#include "anim_test_util.h"
#include "detail/animation_detail.h"

using anim_test::make_sheet;
using anim_test::Rig;
using lam::AnimDirection;
using lam::str_id;

namespace {

Rig rig_with(lam::SpriteSheet sheet, const char *clip) {
    Rig r;
    r.sheet = std::move(sheet);
    r.a.clip = lam::intern(clip);
    return r;
}

// Frames shown on consecutive ticks, one entry per tick, for a tag of 100 ms frames at 60 Hz
// (6 ticks per frame), sampled every 6th tick starting at the start tick.
std::vector<int> every_frame(Rig &r, int frames_to_sample) {
    std::vector<int> out;
    r.tick(); // start tick: first frame
    out.push_back(r.a.frame);
    for (int i = 1; i < frames_to_sample; ++i) {
        r.tick(6);
        out.push_back(r.a.frame);
    }
    return out;
}

} // namespace

TEST_CASE("animation: a looping tag advances by frame durations; a directional clip keeps its place") {
    // At 60 Hz, 0.15 s = 9 ticks and 0.1 s = 6 ticks.
    Rig r;
    r.sheet = make_sheet({100, 100, 100, 100, 100, 100},
                         {{"walk_down", 0, 1}, {"walk_up", 2, 3}, {"walk_side", 4, 5}});
    lam::AnimClip walk;
    walk.name = lam::intern("walk");
    walk.dir_count = 4; // right, down, left, up: side art faces right, left mirrors it
    walk.tags = {str_id("walk_side"), str_id("walk_down"), {}, str_id("walk_up")};
    r.graph.clips.push_back(walk);
    r.use_graph = true;
    r.a.clip = walk.name;
    r.a.facing = 1; // down
    r.tick();       // start tick shows the first frame
    CHECK(r.a.frame == 0);
    r.tick(9);
    CHECK(r.a.frame == 1);
    CHECK(r.a.elapsed_milliticks == 3000);
    r.a.facing = 2; // left: turning keeps the step
    r.tick(0);
    r.tick();       // 1 more tick: 4000 of 6000
    CHECK(r.a.frame == 5);
    CHECK((r.sprite.flags & lam::kFlipX) != 0);
    r.tick(5);
    CHECK(r.a.frame == 4); // looped
    CHECK(r.a.plays == 1); // one pass completed (the side tag's)
}

TEST_CASE("animator: frames last exactly duration_ms * tick_rate milliticks (no drift)") {
    Rig r = rig_with(make_sheet({100, 50, 17}, {{"t", 0, 2}}), "t");
    std::vector<int> shown = r.frames(1 + 6 + 3 + 2);
    // Start tick, then 6 ticks of frame 0 in total, 3 of frame 1, 17 ms = 1.02 ticks of frame 2.
    CHECK(shown == std::vector<int>{0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 0});
    // 167 ms per loop is 10.02 ticks: 100 loops are exactly 1002 ticks from the start.
    Rig long_run = rig_with(make_sheet({100, 50, 17}, {{"t", 0, 2}}), "t");
    long_run.tick(1 + 1002);
    CHECK(long_run.a.plays == 100);
    CHECK(long_run.a.frame == 0);
    CHECK(long_run.a.elapsed_milliticks == 0);

    SUBCASE("other tick rates") {
        Rig slow = rig_with(make_sheet({100, 100}, {{"t", 0, 1}}), "t");
        slow.tick_rate = 30; // 100 ms = 3 ticks
        CHECK(slow.frames(1 + 3 + 3) == std::vector<int>{0, 0, 0, 1, 1, 1, 0});
        Rig fast = rig_with(make_sheet({10, 10}, {{"t", 0, 1}}), "t");
        fast.tick_rate = 240; // 10 ms = 2.4 ticks
        fast.tick(1 + 24);    // 100 ms = 10 frames of 10 ms -> 5 loops
        CHECK(fast.a.plays == 5);
        CHECK(fast.a.frame == 0);
        CHECK(fast.a.elapsed_milliticks == 0);
    }
}

TEST_CASE("animator: speed scales time exactly; zero, negative and NaN freeze it") {
    Rig r = rig_with(make_sheet({100, 100, 100, 100}, {{"t", 0, 3}}), "t");
    r.a.speed = 2;
    CHECK(r.frames(1 + 3 + 3) == std::vector<int>{0, 0, 0, 1, 1, 1, 2});
    r.a.speed = 0.5f;
    r.tick(11);
    CHECK(r.a.frame == 2);
    r.tick();
    CHECK(r.a.frame == 3);
    for (float s : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::infinity()}) {
        r.a.speed = s;
        const std::uint32_t before = r.a.elapsed_milliticks;
        r.tick(100);
        CHECK(r.a.frame == 3);
        CHECK(r.a.elapsed_milliticks == before);
    }
    CHECK(lam::detail::speed_milliticks(1.0f) == 1000);
    CHECK(lam::detail::speed_milliticks(1.2345f) == 1235);
    CHECK(lam::detail::speed_milliticks(1e9f) == 1000000);
}

TEST_CASE("animator: at most kMaxFrameStepsPerTick frames per tick") {
    Rig r = rig_with(make_sheet({1, 1}, {{"t", 0, 1}}), "t"); // 1 ms = 60 milliticks
    r.a.speed = 1000;                                            // 1e6 milliticks per tick
    r.tick();                                                    // start
    r.tick();
    CHECK(r.a.plays == lam::kMaxFrameStepsPerTick / 2);
    CHECK(r.a.elapsed_milliticks == 0); // the rest of the tick's time was dropped
}

TEST_CASE("animator: forward, reverse and ping-pong order; ping-pong ends are not doubled") {
    const auto order = [](AnimDirection dir, int samples) {
        Rig r = rig_with(make_sheet({100, 100, 100, 100}, {{"t", 0, 2, dir}}), "t");
        return every_frame(r, samples);
    };
    CHECK(order(AnimDirection::Forward, 7) == std::vector<int>{0, 1, 2, 0, 1, 2, 0});
    CHECK(order(AnimDirection::Reverse, 7) == std::vector<int>{2, 1, 0, 2, 1, 0, 2});
    CHECK(order(AnimDirection::PingPong, 9) == std::vector<int>{0, 1, 2, 1, 0, 1, 2, 1, 0});
    CHECK(order(AnimDirection::PingPongReverse, 9) == std::vector<int>{2, 1, 0, 1, 2, 1, 0, 1, 2});
    // A tag in the middle of the sheet uses its own range.
    Rig mid = rig_with(make_sheet({100, 100, 100, 100, 100}, {{"t", 2, 4, AnimDirection::PingPong}}), "t");
    CHECK(every_frame(mid, 6) == std::vector<int>{2, 3, 4, 3, 2, 3});
    // A one-frame ping-pong stays put and counts passes.
    Rig one = rig_with(make_sheet({100}, {{"t", 0, 0, AnimDirection::PingPong}}), "t");
    CHECK(every_frame(one, 4) == std::vector<int>{0, 0, 0, 0});
    CHECK(one.a.plays == 3);
}

TEST_CASE("animator: repeat plays N passes then holds the last frame shown") {
    const auto run = [](AnimDirection dir, std::uint16_t repeat, int samples) {
        Rig r = rig_with(make_sheet({100, 100, 100}, {{"t", 0, 2, dir, repeat}}), "t");
        std::vector<int> f = every_frame(r, samples);
        CHECK(r.a.finished);
        CHECK(r.a.plays == repeat);
        return f;
    };
    CHECK(run(AnimDirection::Forward, 2, 8) == std::vector<int>{0, 1, 2, 0, 1, 2, 2, 2});
    CHECK(run(AnimDirection::Reverse, 1, 5) == std::vector<int>{2, 1, 0, 0, 0});
    // Ping-pong: every sweep is a pass (Aseprite).
    CHECK(run(AnimDirection::PingPong, 1, 5) == std::vector<int>{0, 1, 2, 2, 2});
    CHECK(run(AnimDirection::PingPong, 2, 7) == std::vector<int>{0, 1, 2, 1, 0, 0, 0});
    CHECK(run(AnimDirection::PingPong, 3, 9) == std::vector<int>{0, 1, 2, 1, 0, 1, 2, 2, 2});
    CHECK(run(AnimDirection::PingPongReverse, 2, 7) == std::vector<int>{2, 1, 0, 1, 2, 2, 2});

    SUBCASE("finish happens on the tick the last frame's time is up") {
        Rig r = rig_with(make_sheet({100, 100}, {{"t", 0, 1, AnimDirection::Forward, 1}}), "t");
        r.tick(1 + 6 + 5);
        CHECK_FALSE(r.a.finished);
        r.tick();
        CHECK(r.a.finished);
        CHECK(r.a.frame == 1);
        CHECK(r.a.elapsed_milliticks == 0);
    }
}

TEST_CASE("animator: playing = false freezes time and events") {
    Rig r = rig_with(make_sheet({100, 100}, {{"t", 0, 1}}), "t");
    r.tick();
    r.a.playing = false;
    r.tick(100);
    CHECK(r.a.frame == 0);
    CHECK(r.a.elapsed_milliticks == 0);
    r.a.playing = true;
    r.tick(6);
    CHECK(r.a.frame == 1);
}

TEST_CASE("animator: play() positions at once; the system starts it; restart is explicit") {
    lam::SpriteSheet sheet = make_sheet({100, 100, 100, 100},
                                        {{"idle", 0, 1}, {"back", 2, 3, AnimDirection::Reverse}});
    Rig r;
    r.sheet = sheet;
    lam::play(r.a, r.sheet, str_id("back"));
    CHECK(r.a.clip == str_id("back"));
    CHECK(r.a.tag == -1); // start pending
    CHECK(r.a.frame == 3); // reverse tags start at `to`
    r.tick();
    CHECK(r.a.tag == 1);
    CHECK(r.a.frame == 3);
    r.tick(6);
    CHECK(r.a.frame == 2);

    lam::play(r.a, r.sheet, str_id("back")); // already playing: nothing changes
    CHECK(r.a.frame == 2);
    CHECK(r.a.tag == 1);
    lam::play(r.a, r.sheet, str_id("back"), true);
    CHECK(r.a.frame == 3);
    CHECK(r.a.tag == -1);

    lam::play(r.a, r.sheet, str_id("missing"));
    CHECK(r.a.clip == str_id("missing"));
    r.tick(20); // unknown tag: the animator keeps showing its frame
    CHECK(r.a.frame == 3);

    SUBCASE("writing clip directly restarts too") {
        r.a.clip = str_id("idle");
        r.tick();
        CHECK(r.a.tag == 0);
        CHECK(r.a.frame == 0);
    }
}

TEST_CASE("animator: the sprite gets the frame's region and the flip flag") {
    Rig r = rig_with(make_sheet({100, 100}, {{"t", 0, 1}}), "t");
    r.sheet.texture = lam::AssetId<lam::Texture>{4, 1};
    r.sprite.flags = lam::kYSort;
    r.tick(1 + 6);
    CHECK(r.sprite.region.texture == r.sheet.texture);
    CHECK(r.sprite.region.source == lam::Rect{16, 0, 16, 16});
    CHECK(r.sprite.region.size == lam::Vec2{16, 16});
    CHECK(r.sprite.flags == lam::kYSort);
    r.a.facing = 1; // no graph: 2 facings, 1 = left (mirrored)
    r.tick();
    CHECK(r.sprite.flags == (lam::kYSort | lam::kFlipX));
    r.a.facing = 0;
    r.tick();
    CHECK(r.sprite.flags == lam::kYSort);

    SUBCASE("a sheet with no tags shows its frame (bare .png sheets)") {
        Rig still;
        still.sheet = lam::make_single_frame_sheet({8, 12}, "x.png");
        still.tick(3);
        CHECK(still.sprite.region.source == lam::Rect{0, 0, 8, 12});
        CHECK(still.a.tag == -1);
    }
}

TEST_CASE("facing_index: 2, 4 and 8 directions; zero and ties keep the current facing") {
    using lam::facing_index;
    CHECK(facing_index({1, 0}, 1, 0) == 0);
    CHECK(facing_index({-1, 0}, 1, 0) == 0);
    // 2: right, left; vertical input keeps the facing.
    CHECK(facing_index({1, 0}, 2, 1) == 0);
    CHECK(facing_index({-0.2f, 5}, 2, 0) == 1);
    CHECK(facing_index({0, 1}, 2, 1) == 1);
    CHECK(facing_index({0, 0}, 2, 1) == 1);
    // 4: right, down, left, up (+y down).
    CHECK(facing_index({1, 0.2f}, 4, 3) == 0);
    CHECK(facing_index({0.1f, 1}, 4, 0) == 1);
    CHECK(facing_index({-1, 0}, 4, 0) == 2);
    CHECK(facing_index({0, -1}, 4, 0) == 3);
    CHECK(facing_index({1, 1}, 4, 1) == 1);  // exact diagonal: keep down
    CHECK(facing_index({1, 1}, 4, 0) == 0);  // keep right
    CHECK(facing_index({1, 1}, 4, 3) == 0);  // neither: horizontal wins
    CHECK(facing_index({-1, -1}, 4, 3) == 3);
    // 8: right, down-right, down, down-left, left, up-left, up, up-right.
    CHECK(facing_index({1, 0}, 8, 4) == 0);
    CHECK(facing_index({1, 1}, 8, 0) == 1);
    CHECK(facing_index({0, 1}, 8, 0) == 2);
    CHECK(facing_index({-1, 1}, 8, 0) == 3);
    CHECK(facing_index({-1, 0.3f}, 8, 0) == 4);
    CHECK(facing_index({-1, -1}, 8, 0) == 5);
    CHECK(facing_index({0.3f, -1}, 8, 0) == 6);
    CHECK(facing_index({1, -1}, 8, 0) == 7);
    // Non-finite input and an out-of-range current.
    CHECK(facing_index({std::numeric_limits<float>::quiet_NaN(), 1}, 8, 5) == 5);
    CHECK(facing_index({0, 0}, 4, 7) == 0);
    // Other counts round down to 1, 2, 4 or 8.
    CHECK(facing_index({0, -1}, 5, 0) == 3);
    CHECK(facing_index({-1, 0}, 3, 0) == 1);
    CHECK(facing_index({-1, -1}, 200, 0) == 5);
}

TEST_CASE("clip_tag: mirroring, fallbacks and facing spaces") {
    lam::AnimClip c;
    c.dir_count = 4;
    c.tags = {str_id("side"), str_id("down"), {}, str_id("up")};
    bool flip = false;
    CHECK(lam::clip_tag(c, 0, 4, &flip) == str_id("side"));
    CHECK_FALSE(flip);
    CHECK(lam::clip_tag(c, 2, 4, &flip) == str_id("side"));
    CHECK(flip);
    CHECK(lam::clip_tag(c, 3, 4, &flip) == str_id("up"));
    CHECK_FALSE(flip);
    // Without mirroring, the explicit left tag is used; a missing one still mirrors.
    c.mirror_left = false;
    c.tags[2] = str_id("left");
    CHECK(lam::clip_tag(c, 2, 4, &flip) == str_id("left"));
    CHECK_FALSE(flip);
    c.tags[2] = {};
    CHECK(lam::clip_tag(c, 2, 4, &flip) == str_id("side"));
    CHECK(flip);
    // 8-direction facings on a 4-direction clip: diagonals prefer horizontal.
    c.mirror_left = true;
    CHECK(lam::clip_tag(c, 1, 8, &flip) == str_id("side")); // down-right -> right
    CHECK(lam::clip_tag(c, 2, 8, &flip) == str_id("down"));
    CHECK(lam::clip_tag(c, 5, 8, &flip) == str_id("side")); // up-left -> left (mirrored)
    CHECK(flip);
    CHECK(lam::clip_tag(c, 6, 8, &flip) == str_id("up"));
    // A 1-direction clip flips whenever the facing points left (platformer clips).
    lam::AnimClip one;
    one.tags[0] = str_id("jump");
    CHECK(lam::clip_tag(one, 1, 2, &flip) == str_id("jump"));
    CHECK(flip);
    CHECK(lam::clip_tag(one, 2, 4, &flip) == str_id("jump"));
    CHECK(flip);
    CHECK(lam::clip_tag(one, 1, 4, &flip) == str_id("jump")); // down: not left
    CHECK_FALSE(flip);
    one.mirror_left = false;
    CHECK(lam::clip_tag(one, 1, 2, &flip) == str_id("jump"));
    CHECK_FALSE(flip);
    // 8-direction clip with mirrored left half.
    lam::AnimClip eight;
    eight.dir_count = 8;
    eight.tags = {str_id("e"), str_id("se"), str_id("s"), {}, {}, {}, str_id("n"), str_id("ne")};
    CHECK(lam::clip_tag(eight, 3, 8, &flip) == str_id("se"));
    CHECK(flip);
    CHECK(lam::clip_tag(eight, 4, 8, &flip) == str_id("e"));
    CHECK(lam::clip_tag(eight, 5, 8, &flip) == str_id("ne"));
    CHECK(flip);
    CHECK(lam::clip_tag(eight, 2, 8, nullptr) == str_id("s"));

    CHECK(lam::detail::convert_facing(3, 4, 2) == 0);
    CHECK(lam::detail::convert_facing(2, 4, 2) == 1);
    CHECK(lam::detail::convert_facing(1, 2, 4) == 2);
    CHECK(lam::detail::facing_is_left(1, 2));
    CHECK_FALSE(lam::detail::facing_is_left(0, 1));
}

TEST_CASE("params: set, get and capacity; triggers: no duplicates, four at most") {
    lam::AnimParams p;
    CHECK(lam::get_param(p, str_id("speed")) == 0.0f);
    lam::set_param(p, str_id("speed"), 3.5f);
    lam::set_param(p, str_id("vy"), -2);
    lam::set_param(p, str_id("speed"), 4);
    CHECK(lam::get_param(p, str_id("speed")) == 4.0f);
    CHECK(lam::get_param(p, str_id("vy")) == -2.0f);
    CHECK(p.values[0].name == str_id("speed")); // slots keep their order
    CHECK(p.values[1].name == str_id("vy"));
    CHECK(lam::get_param(p, lam::StrId{}) == 0.0f);

    lam::fire_trigger(p, str_id("a"));
    lam::fire_trigger(p, str_id("a"));
    lam::fire_trigger(p, str_id("b"));
    lam::fire_trigger(p, str_id("c"));
    lam::fire_trigger(p, str_id("d"));
    lam::fire_trigger(p, str_id("e")); // dropped
    lam::fire_trigger(p, lam::StrId{}); // ignored
    CHECK(p.triggers[0] == str_id("a"));
    CHECK(p.triggers[1] == str_id("b"));
    CHECK(p.triggers[3] == str_id("d"));
}

TEST_CASE("current_slice: keyed by frame, relative to the pivot, mirrored with the sprite") {
    lam::SpriteSheet s = make_sheet({100, 100, 100, 100}, {{"t", 0, 3}});
    lam::SheetSlice hit;
    hit.name = str_id("hit");
    hit.keys = {lam::SliceKey{1, lam::Rect{10, 4, 4, 2}, {}}, lam::SliceKey{3, lam::Rect{0, 0, 0, 0}, {}}};
    s.slices.push_back(hit);
    lam::Animator a;
    a.frame = 0;
    CHECK_FALSE(lam::current_slice(a, s, str_id("hit"), false)); // before the first key
    a.frame = 1;
    // 16x16 frame, pivot (0.5, 1) = (8, 16): x 10 -> 2, y 4 -> -12.
    CHECK(lam::current_slice(a, s, str_id("hit"), false) == lam::Rect{2, -12, 4, 2});
    // Mirrored: the slice spans 16 - 10 - 4 = 2..6 in the flipped frame -> -6.
    CHECK(lam::current_slice(a, s, str_id("hit"), true) == lam::Rect{-6, -12, 4, 2});
    a.frame = 2; // key 1 still applies
    CHECK(lam::current_slice(a, s, str_id("hit"), false) == lam::Rect{2, -12, 4, 2});
    CHECK(lam::current_slice(a, s, str_id("hit"), false, lam::Vec2{0, 0}) == lam::Rect{10, 4, 4, 2});
    // Pivot (0, 0): the flipped sprite covers x -16..0, the slice -14..-10 (mirrored about x = 0).
    CHECK(lam::current_slice(a, s, str_id("hit"), true, lam::Vec2{0, 0}) == lam::Rect{-14, 4, 4, 2});
    a.frame = 3; // an empty key hides the slice
    CHECK_FALSE(lam::current_slice(a, s, str_id("hit"), false));
    CHECK_FALSE(lam::current_slice(a, s, str_id("hurt"), false));
    a.frame = 99;
    CHECK_FALSE(lam::current_slice(a, s, str_id("hit"), false));
}

// ---- Regressions --------------------------------------------------------------------------------

TEST_CASE("a flipped slice is mirrored about the sprite pivot, as the sprite is drawn") {
    lam::SpriteSheet s = make_sheet({100}, {{"t", 0, 0}});
    s.frames[0].source_size = {32, 32};
    lam::SheetSlice hit;
    hit.name = str_id("hit");
    hit.keys = {lam::SliceKey{0, lam::Rect{20, 4, 8, 6}, {}}};
    s.slices.push_back(hit);
    lam::Animator a;
    const lam::Vec2 pivot{0.25f, 1.0f}; // px = 8, py = 32
    CHECK(lam::current_slice(a, s, str_id("hit"), false, pivot) == lam::Rect{12, -28, 8, 6});
    // Drawn flipped, texel u lands at x = 8 - u: the slice covers -20..-12 (was -4..4).
    CHECK(lam::current_slice(a, s, str_id("hit"), true, pivot) == lam::Rect{-20, -28, 8, 6});
    // The default pivot (0.5, 1) is unchanged: 16 - 20 - 8 = -12.
    CHECK(lam::current_slice(a, s, str_id("hit"), true) == lam::Rect{-12, -28, 8, 6});
}
