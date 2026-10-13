// Energy turn scheduler: deterministic order, budget, busy, player waits, snapshots.
#include "ecs_test.h"
#include <lam/ecs/snapshot.h>
#include <lam/ecs/turns.h>
#include <lam/ecs/world.h>
#include <vector>

using namespace lam;

TEST_CASE("actors act by energy (desc), ties by entity index; speed grants energy") {
    World w;
    add_turn_scheduler(w);
    const Entity a = w.create(), b = w.create(), c = w.create();
    w.add<TurnActor>(c, TurnActor{0, 100});
    w.add<TurnActor>(b, TurnActor{0, 200});
    w.add<TurnActor>(a, TurnActor{0, 100}); // dense order c b a: must not matter
    std::vector<Entity> order;
    for (int i = 0; i < 6; ++i) {
        const Entity e = next_actor(w);
        REQUIRE(!e.is_null());
        CHECK(next_actor(w) == e); // still its turn until end_turn
        order.push_back(e);
        end_turn(w, e);
    }
    // b reaches 1000 first (5 rounds); then a, b, c all at 1000 -> a, b, c by index; then b...
    CHECK(order == std::vector<Entity>{b, a, b, c, b, a});
    CHECK(w.resources().get<TurnScheduler>().turns == 6);
}

TEST_CASE("per-tick budget, busy flag and no actors") {
    World w;
    TurnScheduler &s = add_turn_scheduler(w);
    CHECK(next_actor(w).is_null()); // nobody to act
    s.max_actors_per_tick = 2;
    const Entity a = w.create();
    w.add<TurnActor>(a, TurnActor{5000, 100});
    end_turn(w, next_actor(w));
    end_turn(w, next_actor(w));
    CHECK(next_actor(w).is_null()); // budget spent this tick
    w.advance_tick();
    CHECK(next_actor(w) == a);
    set_turns_busy(w, true);
    CHECK(next_actor(w).is_null());
    set_turns_busy(w, false);
    CHECK(next_actor(w) == a);
    const Entity stuck = w.create();
    World w2;
    add_turn_scheduler(w2);
    w2.add<TurnActor>(w2.create(), TurnActor{0, 0}); // speed 0: can never act
    CHECK(next_actor(w2).is_null());
    (void)stuck;
}

TEST_CASE("the typical loop stops at the player and resumes after its end_turn") {
    World w;
    add_turn_scheduler(w);
    const Entity player = w.create(), orc = w.create();
    w.add<TurnActor>(player, TurnActor{0, 100});
    w.add<PlayerTurn>(player);
    w.add<TurnActor>(orc, TurnActor{0, 100});
    int orc_turns = 0;
    auto run = [&] {
        for (Entity e; (e = next_actor(w)) && !w.has<PlayerTurn>(e);) {
            ++orc_turns;
            end_turn(w, e);
        }
    };
    run();
    CHECK(next_actor(w) == player); // waiting for input
    CHECK(orc_turns == 0);          // player has the lower index
    end_turn(w, player);            // input arrived
    run();
    CHECK(orc_turns == 1);
    CHECK(next_actor(w) == player);
}

TEST_CASE("the scheduler is snapshotted and hashed") {
    World w;
    add_turn_scheduler(w);
    const Entity a = w.create();
    w.add<TurnActor>(a, TurnActor{0, 100});
    WorldSnapshot s;
    w.snapshot_into(s);
    const std::uint64_t h = w.state_hash();
    end_turn(w, next_actor(w));
    CHECK(w.state_hash() != h);
    w.restore(s);
    CHECK(w.state_hash() == h);
    CHECK(w.resources().get<TurnScheduler>().turns == 0);
    CHECK(w.get<TurnActor>(a).energy == 0);
}

TEST_CASE("peek_actor answers like next_actor without granting energy or setting current") {
    World w;
    CHECK(peek_actor(w).is_null()); // no scheduler: null (next_actor would assert)
    TurnScheduler &s = add_turn_scheduler(w);
    CHECK(peek_actor(w).is_null()); // no actors
    const Entity a = w.create(), b = w.create(), slow = w.create();
    w.add<TurnActor>(a, TurnActor{0, 100});
    w.add<TurnActor>(b, TurnActor{0, 200});
    w.add<TurnActor>(slow, TurnActor{0, 30});
    const std::uint64_t h = w.state_hash();
    CHECK(peek_actor(w) == b); // b would reach the threshold first (5 rounds)
    CHECK(w.state_hash() == h); // read-only: no energy granted, no current, no counters
    CHECK(s.current.is_null());
    CHECK(w.get<TurnActor>(b).energy == 0);
    CHECK(s.acted_this_tick == 0);
    for (int i = 0; i < 12; ++i) {
        const Entity peeked = peek_actor(w);
        const Entity e = next_actor(w);
        REQUIRE(!e.is_null());
        CHECK(peeked == e);
        CHECK(peek_actor(w) == e); // `current` until end_turn
        end_turn(w, e);
    }
    // Busy, and a spent per-tick budget, are null for both.
    set_turns_busy(w, true);
    CHECK(peek_actor(w).is_null());
    set_turns_busy(w, false);
    s.max_actors_per_tick = s.acted_this_tick;
    CHECK(peek_actor(w).is_null());
    CHECK(next_actor(w).is_null());
    w.advance_tick(); // the budget rolls with the tick
    const Entity next = peek_actor(w);
    CHECK(!next.is_null());
    CHECK(next_actor(w) == next);
    // The actor waiting for input stays `current` across ticks, whatever the budget.
    w.advance_tick();
    s.max_actors_per_tick = 0;
    CHECK(peek_actor(w) == next);
    // Actors that can never act: null.
    World stuck;
    add_turn_scheduler(stuck);
    stuck.add<TurnActor>(stuck.create(), TurnActor{0, 0});
    CHECK(peek_actor(stuck).is_null());
}
