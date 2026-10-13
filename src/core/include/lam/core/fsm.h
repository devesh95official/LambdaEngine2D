#pragma once
// Enum-plus-timer state machine for component fields. Call tick() once per tick after the state
// logic; a new state's k-th tick sees ticks_in_state == k. For reflection see lam/ecs/reflect_fsm.h.
//   if (ai.fsm.in(Wolf::Chase) && ai.fsm.ticks_in_state > 120) ai.fsm.set(Wolf::Wander);
//   ai.fsm.tick();
#include <cstdint>
namespace lam {
template <class State> struct Fsm {
    State state{};
    State previous{};
    std::uint32_t ticks_in_state = 0;
    bool entered = true;                                        // true from set() until the next tick()
    // No-op if already in s.
    void set(State s) {
        if (s == state) return;
        previous = state;
        state = s;
        ticks_in_state = 0;
        entered = true;
    }
    void tick() {
        ++ticks_in_state;
        entered = false;
    }
    bool in(State s) const { return state == s; }
};
}
