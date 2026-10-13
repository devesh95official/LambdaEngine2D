// Profiler timings and LAM_PROFILE_SCOPE.
#include <doctest/doctest.h>

#include <chrono>
#include <string_view>
#include <thread>

#include <lam/core/profiler.h>
#include <lam/test/support.h>

using namespace lam;

namespace {
void spin_for_us(int us) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end) {}
}
} // namespace

TEST_CASE("profiler: timings appear in first-seen order after end_frame") {
    Profiler p;
    CHECK(p.timings().empty());
    p.begin("update");
    spin_for_us(200);
    p.end();
    p.begin("render");
    p.begin("render.sprites");
    spin_for_us(200);
    p.end();
    p.end();
    CHECK(p.timings().empty()); // nothing published before end_frame
    p.end_frame();
    const auto t = p.timings();
    REQUIRE(t.size() == 3);
    CHECK(t[0].name == "update");
    CHECK(t[1].name == "render");
    CHECK(t[2].name == "render.sprites");
    CHECK(t[0].ms > 0.1);
    CHECK(t[1].ms >= t[2].ms); // nested time is part of the parent
    for (const StageTiming &s : t)
        CHECK(s.max_ms >= s.ms);
}

TEST_CASE("profiler: each frame reports itself; max_ms remembers the worst") {
    Profiler p;
    p.begin("work");
    spin_for_us(2000);
    p.end();
    p.end_frame();
    const double first = p.timings()[0].ms;
    p.begin("work");
    p.end();
    p.end_frame();
    REQUIRE(p.timings().size() == 1);
    CHECK(p.timings()[0].ms < first);
    CHECK(p.timings()[0].max_ms >= first);
    p.end_frame(); // a frame without the scope reports 0
    CHECK(p.timings()[0].ms == 0.0);
    p.end(); // unbalanced end is ignored
    p.reset();
    p.end_frame();
    CHECK(p.timings().empty());
}

TEST_CASE("profiler: a warmed-up frame does not allocate") {
    Profiler p;
    auto frame = [&p] {
        p.begin("a");
        p.begin("b");
        p.end();
        p.end();
        p.begin("c");
        p.end();
        p.end_frame();
    };
    frame();
    CHECK_NO_ALLOC(frame());
    CHECK_NO_ALLOC(frame());
}

TEST_CASE("profiler: LAM_PROFILE_SCOPE records into the current profiler") {
    Profiler p;
    detail::set_current_profiler(&p);
    {
        LAM_PROFILE_SCOPE("scoped");
        spin_for_us(100);
    }
    detail::set_current_profiler(nullptr);
    {
        LAM_PROFILE_SCOPE("ignored"); // no current profiler: nothing happens
    }
    p.end_frame();
#if (defined(LAM_DEV) && LAM_DEV) || (defined(LAM_PROFILE) && LAM_PROFILE)
    REQUIRE(p.timings().size() == 1);
    CHECK(p.timings()[0].name == "scoped");
    CHECK(p.timings()[0].ms > 0.0);
#else
    CHECK(p.timings().empty());
#endif
    CHECK(detail::current_profiler() == nullptr);
}
