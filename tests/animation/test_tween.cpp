// Tick-time Tween and frame-time FrameTween. Most cases call detail::tick_tween and
// tick_frame_tween directly; the App cases cover the systems.
#include <cmath>
#include <limits>
#include <vector>

#include <lam/animation/tween.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/render/sprite.h>

#include "anim_test_util.h"

using lam::Ease;
using lam::TweenSpec;
using lam::TweenTarget;
using lam::str_id;

namespace {

struct Done {
    std::vector<lam::StrId> tags;
    std::vector<bool> last;
    static void sink(void *user, lam::StrId tag, bool last) {
        auto *d = static_cast<Done *>(user);
        d->tags.push_back(tag);
        d->last.push_back(last);
    }
};

lam::Tween one_step(const TweenSpec &s) {
    lam::Tween t;
    t.steps[0] = s;
    t.count = 1;
    return t;
}

TweenSpec linear_x(float from, float to, std::uint16_t ticks) {
    TweenSpec s;
    s.target = TweenTarget::Position;
    s.from = {from, 0};
    s.to = {to, 0};
    s.ticks = ticks;
    s.ease = Ease::Linear;
    return s;
}

// x after each tick until the tween finishes (at most `limit` ticks).
std::vector<float> run_x(lam::Tween &t, lam::Transform &xf, Done *done = nullptr, int limit = 100) {
    std::vector<float> xs;
    for (int i = 0; i < limit; ++i) {
        const bool finished = lam::detail::tick_tween(t, &xf, nullptr, done ? &Done::sink : nullptr, done);
        xs.push_back(xf.position.x);
        if (finished) break;
    }
    return xs;
}

} // namespace

TEST_CASE("tween: a linear step reaches its end after exactly `ticks` ticks") {
    lam::Tween t = one_step(linear_x(0, 10, 10));
    t.steps[0].tag = str_id("moved");
    lam::Transform xf;
    Done done;
    CHECK(run_x(t, xf, &done) == std::vector<float>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
    REQUIRE(done.tags.size() == 1);
    CHECK(done.tags[0] == str_id("moved"));
    CHECK(done.last[0]);
    CHECK(t.current == 1);
    // A finished tween stays finished.
    CHECK(lam::detail::tick_tween(t, &xf, nullptr, nullptr, nullptr));
    CHECK(xf.position.x == 10);
}

TEST_CASE("tween: easing, delay and zero-length steps") {
    lam::Tween t = one_step(linear_x(0, 10, 10));
    t.steps[0].ease = Ease::OutQuad;
    lam::Transform xf;
    const std::vector<float> xs = run_x(t, xf);
    CHECK(xs[4] == doctest::Approx(7.5f)); // OutQuad(0.5) = 0.75
    CHECK(xs.back() == 10);

    lam::Tween d = one_step(linear_x(0, 2, 2));
    d.steps[0].delay = 3;
    lam::Transform at{.position = {9, 9}};
    CHECK(run_x(d, at) == std::vector<float>{9, 9, 9, 1, 2}); // nothing written while waiting

    lam::Tween z = one_step(linear_x(0, 5, 0));
    lam::Transform zx;
    CHECK(run_x(z, zx) == std::vector<float>{5});

    // delay + ticks is clamped to 65535 (elapsed is 16-bit).
    lam::Tween big = one_step(linear_x(0, 1, 65535));
    big.steps[0].delay = 100;
    lam::Transform bx;
    lam::detail::tick_tween(big, &bx, nullptr, nullptr, nullptr);
    CHECK(bx.position.x > 0.0f);
}

TEST_CASE("tween: repeat plays repeat + 1 passes; yoyo runs odd passes backwards") {
    lam::Tween r = one_step(linear_x(0, 4, 4));
    r.steps[0].repeat = 2;
    lam::Transform xf;
    CHECK(run_x(r, xf) == std::vector<float>{1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4});

    lam::Tween y = one_step(linear_x(0, 4, 4));
    y.steps[0].repeat = 1;
    y.steps[0].yoyo = true;
    lam::Transform yx;
    CHECK(run_x(y, yx) == std::vector<float>{1, 2, 3, 4, 3, 2, 1, 0});

    SUBCASE("repeat 255 loops forever, delay only before the first pass") {
        lam::Tween f = one_step(linear_x(0, 2, 2));
        f.steps[0].repeat = 255;
        f.steps[0].yoyo = true;
        f.steps[0].delay = 1;
        lam::Transform fx;
        const std::vector<float> xs = run_x(f, fx, nullptr, 1000);
        CHECK(xs.size() == 1000); // never finished
        CHECK(std::vector<float>(xs.begin(), xs.begin() + 7) ==
              std::vector<float>{0, 1, 2, 1, 0, 1, 2});
        const float pattern[4] = {1, 2, 1, 0}; // after the delay tick: period of 4 ticks
        for (std::size_t i = 1; i < xs.size(); ++i) {
            if (xs[i] != pattern[(i - 1) % 4]) FAIL_CHECK("tick " << i << ": " << xs[i]);
        }
    }
}

TEST_CASE("tween: relative steps start from the target's value when they start") {
    lam::Tween t = one_step(linear_x(0, 5, 5));
    t.steps[0].relative = true;
    t.steps[0].delay = 2;
    lam::Transform xf{.position = {10, 3}};
    lam::detail::tick_tween(t, &xf, nullptr, nullptr, nullptr);
    xf.position.x = 20; // moved during the delay: the base is taken when the step starts
    const std::vector<float> xs = run_x(t, xf);
    CHECK(xs == std::vector<float>{20, 21, 22, 23, 24, 25});
    CHECK(xf.position.y == 3.0f); // from/to y offsets are 0: y kept
    CHECK_FALSE(t.steps[0].relative); // stored as absolute values now
    CHECK(t.steps[0].from == lam::Vec2{20, 3});

    SUBCASE("relative alpha, rotation and custom") {
        lam::Sprite sp;
        sp.tint.a = 51; // 0.2
        TweenSpec a;
        a.target = TweenTarget::Alpha;
        a.from = {0, 0};
        a.to = {0.4f, 0};
        a.ticks = 1;
        a.ease = Ease::Linear;
        a.relative = true;
        lam::Tween at = one_step(a);
        lam::Transform none;
        CHECK(lam::detail::tick_tween(at, &none, &sp, nullptr, nullptr));
        CHECK(sp.tint.a == 102); // 0.4 * 255: a tick tween's Alpha is absolute (Sprite is not hashed)
        TweenSpec c = a;
        c.target = TweenTarget::Custom;
        c.to = {2, 0};
        lam::Tween ct = one_step(c);
        ct.custom = 1;
        lam::detail::tick_tween(ct, nullptr, nullptr, nullptr, nullptr);
        CHECK(ct.custom == 3.0f);
    }
}

TEST_CASE("tween: every target") {
    lam::Transform xf;
    lam::Sprite sp;
    const auto step = [](TweenTarget target, lam::Vec2 from, lam::Vec2 to) {
        TweenSpec s;
        s.target = target;
        s.from = from;
        s.to = to;
        s.ticks = 2;
        s.ease = Ease::Linear;
        return s;
    };
    lam::Tween scale = one_step(step(TweenTarget::Scale, {1, 1}, {3, 5}));
    lam::detail::tick_tween(scale, &xf, &sp, nullptr, nullptr);
    CHECK(xf.scale == lam::Vec2{2, 3});
    lam::Tween rot = one_step(step(TweenTarget::Rotation, {0, 9}, {1, 9}));
    lam::detail::tick_tween(rot, &xf, &sp, nullptr, nullptr);
    CHECK(xf.rotation == 0.5f);
    lam::Tween alpha = one_step(step(TweenTarget::Alpha, {1, 0}, {0, 0}));
    lam::detail::tick_tween(alpha, &xf, &sp, nullptr, nullptr);
    CHECK(sp.tint.a == 128); // 127.5 rounds up
    lam::detail::tick_tween(alpha, &xf, &sp, nullptr, nullptr);
    CHECK(sp.tint.a == 0);
    TweenSpec tint = step(TweenTarget::Tint, {}, {});
    tint.color_from = lam::Color{0, 0, 0, 255};
    tint.color_to = lam::Color{200, 100, 50, 255};
    lam::Tween tt = one_step(tint);
    lam::detail::tick_tween(tt, &xf, &sp, nullptr, nullptr);
    CHECK(sp.tint == lam::Color{100, 50, 25, 255});
    lam::Tween custom = one_step(step(TweenTarget::Custom, {10, 0}, {20, 0}));
    lam::detail::tick_tween(custom, nullptr, nullptr, nullptr, nullptr);
    CHECK(custom.custom == 15.0f);
    // Missing targets are skipped; time still passes.
    lam::Tween blind = one_step(step(TweenTarget::Tint, {}, {}));
    CHECK_FALSE(lam::detail::tick_tween(blind, nullptr, nullptr, nullptr, nullptr));
    CHECK(lam::detail::tick_tween(blind, nullptr, nullptr, nullptr, nullptr));
}

TEST_CASE("tween: sequences continue without a pause and report tagged steps") {
    lam::Tween t;
    t.steps[0] = linear_x(0, 2, 2);
    t.steps[0].tag = str_id("first");
    t.steps[1] = linear_x(2, 3, 1); // untagged middle step: not reported
    t.steps[2] = linear_x(3, 5, 2);
    t.count = 3;
    lam::Transform xf;
    Done done;
    CHECK(run_x(t, xf, &done) == std::vector<float>{1, 2, 3, 4, 5});
    REQUIRE(done.tags.size() == 3);
    CHECK(done.tags[0] == str_id("first"));
    CHECK_FALSE(done.last[0]);
    CHECK(done.last[2]); // the last step always reports (its tag may be empty)
    CHECK_FALSE(done.tags[2]);

    lam::Tween empty;
    CHECK(lam::detail::tick_tween(empty, &xf, nullptr, nullptr, nullptr));
}

TEST_CASE("tween: the App system writes Transforms, emits TweenFinished and removes the Tween") {
    anim_test::AnimApp a;
    anim_test::EventLog &log = anim_test::log_events(a.app());
    lam::World &w = a.world();
    const lam::Entity e = w.create();
    w.add<lam::Transform>(e);
    TweenSpec s = linear_x(0, 30, 3);
    s.tag = str_id("arrived");
    lam::tween_to(w.commands(), e, s);
    w.commands().apply();
    REQUIRE(w.has<lam::Tween>(e));
    a.ticks(1);
    CHECK(w.get<lam::Transform>(e).position.x == 10);
    a.ticks(2);
    CHECK(w.get<lam::Transform>(e).position.x == 30);
    CHECK_FALSE(w.has<lam::Tween>(e)); // removed by Commands after the Animate stage
    CHECK(log.count_tween(str_id("arrived")) == 0); // read by Simulate on the next tick
    const std::uint64_t finished_tick = w.tick();
    a.ticks(1);
    REQUIRE(log.count_tween(str_id("arrived")) == 1);
    CHECK(log.items.back().tick == finished_tick + 1);
    CHECK(log.items.back().tween.entity == e);

    SUBCASE("tween_to replaces a running tween; an empty sequence removes it") {
        lam::tween_to(w.commands(), e, linear_x(100, 200, 100));
        w.commands().apply();
        a.ticks(1);
        CHECK(w.get<lam::Transform>(e).position.x == 101);
        lam::tween_to(w.commands(), e, linear_x(0, 1, 1));
        w.commands().apply();
        a.ticks(1);
        CHECK(w.get<lam::Transform>(e).position.x == 1);
        lam::tween_sequence(w.commands(), e, std::vector<TweenSpec>{linear_x(1, 9, 50)});
        w.commands().apply();
        CHECK(w.get<lam::Tween>(e).count == 1);
        lam::tween_sequence(w.commands(), e, {});
        w.commands().apply();
        CHECK_FALSE(w.has<lam::Tween>(e));
    }
}

TEST_CASE("tween: a child's tween moves Parent::local, so propagation keeps it") {
    anim_test::AnimApp a;
    lam::World &w = a.world();
    const lam::Entity parent = w.create();
    w.add<lam::Transform>(parent, lam::Transform{.position = {100, 50}});
    const lam::Entity child = w.create();
    w.add<lam::Transform>(child);
    lam::set_parent(w, child, parent, lam::Transform{});
    lam::tween_to(w.commands(), child, linear_x(0, 8, 2));
    w.commands().apply();
    a.ticks(2);
    CHECK(w.get<lam::Parent>(child).local.position.x == 8);
    CHECK(w.get<lam::Transform>(child).position == lam::Vec2{108, 50});
}

TEST_CASE("frame tween: frame time, RenderOffset only, removed when done, never hashed") {
    anim_test::AnimApp a;
    anim_test::AnimApp plain;
    lam::World &w = a.world();
    lam::World &pw = plain.world();
    const lam::Entity e = w.create();
    w.add<lam::Transform>(e, lam::Transform{.position = {5, 5}});
    const lam::Entity pe = pw.create();
    pw.add<lam::Transform>(pe, lam::Transform{.position = {5, 5}});
    a.h.frames(1); // the App's first frame has frame_dt 0 (no previous frame)
    plain.h.frames(1);
    TweenSpec s = linear_x(0, 6, 6); // 6 ticks x tick_dt = 0.1 s
    s.target = TweenTarget::Position;
    lam::frame_tween_to(w.commands(), e, s);
    w.commands().apply();
    REQUIRE(w.has<lam::RenderOffset>(e)); // added because the target needs it
    a.h.frames(3);
    plain.h.frames(3);
    CHECK(w.get<lam::RenderOffset>(e).offset.x == doctest::Approx(3.0f));
    CHECK(w.get<lam::Transform>(e).position == lam::Vec2{5, 5});
    CHECK(w.state_hash() == pw.state_hash());
    a.h.frames(3);
    plain.h.frames(3);
    CHECK(w.get<lam::RenderOffset>(e).offset.x == doctest::Approx(6.0f));
    CHECK_FALSE(w.has<lam::FrameTween>(e));
    CHECK(w.state_hash() == pw.state_hash());

    SUBCASE("a frame tween keeps playing while ticks are held") {
        TweenSpec fade;
        fade.target = TweenTarget::Alpha;
        fade.from = {1, 0};
        fade.to = {0, 0};
        fade.ticks = 4;
        fade.ease = Ease::Linear;
        w.add<lam::Sprite>(e);
        lam::frame_tween_to(w.commands(), e, fade);
        w.commands().apply();
        a.app().set_paused(true);
        const std::uint64_t tick = w.tick();
        a.h.frames(2);
        CHECK(w.tick() == tick);
        CHECK(w.get<lam::Sprite>(e).tint.a == 128);
        a.app().set_paused(false);
    }
}

TEST_CASE("frame tween: large frames carry over steps and passes; zero-length loops are bounded") {
    lam::FrameTween t;
    t.steps[0] = linear_x(0, 1, 1);
    t.steps[1] = linear_x(10, 20, 2);
    t.count = 2;
    lam::RenderOffset off;
    const float dt = 1.0f / 60.0f;
    CHECK_FALSE(lam::detail::tick_frame_tween(t, 2 * dt, dt, &off, nullptr)); // step 0 done + half of step 1
    CHECK(t.current == 1);
    CHECK(off.offset.x == doctest::Approx(15.0f));
    CHECK(lam::detail::tick_frame_tween(t, 5 * dt, dt, &off, nullptr));
    CHECK(off.offset.x == doctest::Approx(20.0f));

    lam::FrameTween forever;
    forever.steps[0] = linear_x(0, 1, 0);
    forever.steps[0].repeat = 255;
    forever.count = 1;
    CHECK_FALSE(lam::detail::tick_frame_tween(forever, dt, dt, &off, nullptr)); // bounded, no hang
    CHECK_FALSE(lam::detail::tick_frame_tween(forever, std::numeric_limits<float>::quiet_NaN(), dt,
                                              &off, nullptr));

    lam::FrameTween scale;
    TweenSpec s = linear_x(0, 0, 2);
    s.target = TweenTarget::Scale;
    s.from = {1, 1};
    s.to = {2, 3};
    scale.steps[0] = s;
    scale.count = 1;
    lam::detail::tick_frame_tween(scale, dt, dt, &off, nullptr);
    CHECK(off.scale.x == doctest::Approx(1.5f));
    CHECK(off.scale.y == doctest::Approx(2.0f));
}

// ---- Regressions --------------------------------------------------------------------------------

TEST_CASE("a FrameTween without its RenderOffset (a loaded save, a prefab) gets one") {
    anim_test::AnimApp a;
    lam::World &w = a.world();
    a.h.frames(1); // the App's first frame has frame_dt 0
    TweenSpec squash;
    squash.target = TweenTarget::Scale;
    squash.from = {1, 1};
    squash.to = {1.5f, 0.5f};
    squash.ticks = 6;
    squash.repeat = 255;
    squash.yoyo = true;
    squash.ease = Ease::Linear;
    SUBCASE("a save/load drops RenderOffset (kSnapshot only) and keeps FrameTween (kSerialize)") {
        const lam::Entity e = w.create();
        w.add<lam::Transform>(e);
        lam::frame_tween_to(w.commands(), e, squash);
        w.commands().apply();
        a.h.frames(3);
        w.remove<lam::RenderOffset>(e);                          // what deserialize_world leaves
        a.h.frames(4);
        REQUIRE(w.has<lam::RenderOffset>(e));                    // was never added again
        CHECK(w.get<lam::RenderOffset>(e).scale != lam::Vec2{1, 1});
    }
    SUBCASE("a FrameTween added as a component (prefab) without RenderOffset") {
        const lam::Entity e = w.create();
        w.add<lam::Transform>(e);
        lam::FrameTween t;
        t.steps[0] = squash;
        t.count = 1;
        w.add<lam::FrameTween>(e, t);
        a.h.frames(4);
        REQUIRE(w.has<lam::RenderOffset>(e));
        CHECK(w.get<lam::RenderOffset>(e).scale != lam::Vec2{1, 1});
    }
}

TEST_CASE("a tick Tween's relative Alpha never reads the (unhashed) Sprite") {
    // Two runs whose frame-time writers left different alphas when the tick tween starts must
    // store the same (hashed) Tween.
    TweenSpec fade;
    fade.target = TweenTarget::Alpha;
    fade.from = {0, 0};
    fade.to = {-1, 0};
    fade.ticks = 4;
    fade.ease = Ease::Linear;
    fade.relative = true;
    lam::Tween live = one_step(fade);
    lam::Tween replay = one_step(fade);
    lam::Sprite flashing;
    flashing.tint.a = 128; // a FrameTween hit flash, mid-way at 144 Hz
    lam::Sprite steady;
    steady.tint.a = 255;   // the same tick in a 1-frame-per-tick replay
    lam::detail::tick_tween(live, nullptr, &flashing, nullptr, nullptr);
    lam::detail::tick_tween(replay, nullptr, &steady, nullptr, nullptr);
    CHECK(live.steps[0].from == replay.steps[0].from);           // was 0.5 vs 1.0
    CHECK(live.steps[0].to == replay.steps[0].to);
    CHECK_FALSE(live.steps[0].relative);
    // A FrameTween (presentation) still takes its relative base from the Sprite.
    lam::FrameTween ft;
    ft.steps[0] = fade;
    ft.count = 1;
    lam::Sprite sp;
    sp.tint.a = 255;
    lam::detail::tick_frame_tween(ft, 0.0f, 1.0f / 60, nullptr, &sp);
    CHECK(ft.steps[0].from.x == doctest::Approx(1.0f));
}
