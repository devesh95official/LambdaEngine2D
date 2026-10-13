// lam_core Fsm<State>: the enum-plus-timer state machine stored in components.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include <lam/core/fsm.h>

using namespace lam;

namespace {
enum class Wolf : std::uint8_t { Idle, Wander, Chase };
}

TEST_CASE("fsm: starts in the default state, entered") {
    Fsm<Wolf> f;
    CHECK(f.in(Wolf::Idle));
    CHECK(f.state == Wolf::Idle);
    CHECK(f.previous == Wolf::Idle);
    CHECK(f.entered);
    CHECK(f.ticks_in_state == 0);
}

TEST_CASE("fsm: tick counts time in the state; set switches and resets") {
    Fsm<Wolf> f;
    f.tick();
    f.tick();
    CHECK(f.ticks_in_state == 2);
    CHECK_FALSE(f.entered);
    f.set(Wolf::Chase);
    CHECK(f.in(Wolf::Chase));
    CHECK(f.previous == Wolf::Idle);
    CHECK(f.ticks_in_state == 0);
    CHECK(f.entered);
    f.tick();
    CHECK_FALSE(f.entered);
    f.set(Wolf::Chase); // same state: no-op
    CHECK(f.ticks_in_state == 1);
    CHECK_FALSE(f.entered);
    CHECK(f.previous == Wolf::Idle);
    f.set(Wolf::Wander);
    CHECK(f.previous == Wolf::Chase);
    // The usual pattern: switch after a timeout.
    for (int i = 0; i < 121; ++i) {
        if (f.in(Wolf::Wander) && f.ticks_in_state > 120) f.set(Wolf::Idle);
        f.tick();
    }
    CHECK(f.in(Wolf::Wander));
    if (f.in(Wolf::Wander) && f.ticks_in_state > 120) f.set(Wolf::Idle);
    CHECK(f.in(Wolf::Idle));
}

TEST_CASE("fsm: works with plain integers too") {
    Fsm<int> f;
    f.set(3);
    CHECK(f.in(3));
    CHECK(f.previous == 0);
}

TEST_CASE("fsm: documented timing with tick() after the state logic") {
    // Entry work goes right after set(): `entered` is true only until that tick's tick() call.
    // The new state's logic first runs on the next tick and sees ticks_in_state == 1.
    Fsm<Wolf> f;
    std::vector<std::uint32_t> chase_ticks_seen;
    bool entered_seen_by_logic = false;
    for (int tick = 0; tick < 5; ++tick) {
        if (f.in(Wolf::Idle)) {
            f.set(Wolf::Chase);
            CHECK(f.entered); // entry work here, same tick
        } else if (f.in(Wolf::Chase)) {
            entered_seen_by_logic = entered_seen_by_logic || f.entered;
            chase_ticks_seen.push_back(f.ticks_in_state);
        }
        f.tick();
    }
    CHECK_FALSE(entered_seen_by_logic);
    CHECK(chase_ticks_seen == std::vector<std::uint32_t>{1, 2, 3, 4});
    // An old countdown `timer = 3; if (--timer == 0)` fires on the 3rd tick: ticks_in_state >= 3.
    Fsm<Wolf> g;
    g.set(Wolf::Wander);
    g.tick();
    int fired_on = 0;
    for (int k = 1; k <= 5 && !fired_on; ++k) {
        if (g.ticks_in_state >= 3) fired_on = k;
        g.tick();
    }
    CHECK(fired_on == 3);
}
