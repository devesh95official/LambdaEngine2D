// The Autopilot as an InputDriver on the full (headless) engine: milestones and determinism.
// All need motors, A*, steering and the input driver hook, so they run on DefaultPlugins{headless}.
#include <doctest/doctest.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/physics/components.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_path.h>

#include "forest_fixture.h"
#include "sim/rules.h"

using namespace lantern;
using lantern::test::Forest;

namespace {

// What a minute of bot play looked like, counted at TickEnd (each event exactly once).
struct PlayStats {                                              // App resource (test only)
    int collected = 0, dashes = 0, bites = 0, deaths = 0, windups = 0;
    int bodies_in_trees = 0;                                    // body boxes overlapping a blocking tile
    int routes_served = 0;                                      // PathRoutes written by serve_path_requests
    int chase_ticks = 0;                                        // wolf-ticks spent chasing
    int wander_moves = 0;                                       // wolf-ticks a wanderer actually moved
};

void count_play(lam::Ctx &ctx) {
    PlayStats &s = ctx.res<PlayStats>();
    s.collected += static_cast<int>(ctx.read<LanternCollected>().size());
    s.dashes += static_cast<int>(ctx.read<PlayerDashed>().size());
    s.bites += static_cast<int>(ctx.read<PlayerHurt>().size());
    s.deaths += static_cast<int>(ctx.read<PlayerDied>().size());
    s.windups += static_cast<int>(ctx.read<WolfWindup>().size());
    lam::World &w = ctx.world();
    const lam::TileWorld &tiles = ctx.local<lam::TileWorld>();
    for (auto [e, c, tr] : w.view<lam::Collider, lam::Transform>()) {
        (void)e;
        if (lam::box_blocked(tiles, lam::collider_box(c, tr.position))) ++s.bodies_in_trees;
    }
    for (auto [e, wolf, route] : w.view<Wolf, lam::PathRoute>()) {
        (void)e;
        if (route.count > 0 && route.computed_tick == w.tick()) ++s.routes_served;
        if (wolf.fsm.in(WolfState::Chase)) ++s.chase_ticks;
        if (wolf.fsm.in(WolfState::Wander) && wolf.moving) ++s.wander_moves;
    }
}

} // namespace

TEST_CASE("bot: a minute of play looks like a real run (20-29 lanterns a minute)") {
    Forest f(777, true);
    f.h.app().insert_resource<PlayStats>();
    f.h.app().add_system(lam::Stage::TickEnd, count_play, "lantern.test.count_play");
    f.use_bot();
    f.h.ticks(3600);
    const PlayStats &s = f.h.app().res<PlayStats>();
    MESSAGE("best " << f.run().best << " collected " << s.collected << " dashes " << s.dashes << " bites " << s.bites
                    << " deaths " << s.deaths << " windups " << s.windups << " routes " << s.routes_served
                    << " chase ticks " << s.chase_ticks << " wander moves " << s.wander_moves);
    CHECK(f.run().best >= 15);                                  // 24.3 a minute on average over 300 seeds
    CHECK(s.collected >= f.run().best);                         // every pickup raised an event
    CHECK(s.dashes > 0);                                        // it dodges wind-ups
    CHECK(s.windups > 0);                                       // wolves chase and wind up
    CHECK(s.routes_served > 0);                                 // A* round the trees (PathRequest served)
    CHECK(s.chase_ticks > 0);
    CHECK(s.wander_moves > 0);                                  // wanderers do not stall at their dens
    CHECK(s.bodies_in_trees == 0);                              // trees and rocks block every body
}

TEST_CASE("bot: a death leads to a new run after the restart delay") {
    Forest f(777, true);
    f.use_bot();
    f.h.ticks(4 * 3600);
    MESSAGE("runs " << f.run().runs << " deaths " << f.run().deaths << " best " << f.run().best);
    CHECK(f.run().deaths >= 1);                                 // seed 777 loses a run within 4 minutes
    CHECK(f.run().runs == f.run().deaths + (f.run().phase == Phase::Over ? 0 : 1));
}

TEST_CASE("bot: same seed, same run, bit for bit") {
    Forest a(42, true), b(42, true);
    a.use_bot();
    b.use_bot();
    a.h.ticks(1800);
    b.h.ticks(1800);
    CHECK(a.run().collected == b.run().collected);
    CHECK(a.world().state_hash() == b.world().state_hash());
}

TEST_CASE("bot: an undo snapshot replays identically with a fresh bot") {
    Forest f(9, true);
    f.use_bot();
    f.h.ticks(600);
    lam::WorldSnapshot snap;
    f.world().snapshot_into(snap);
    f.use_bot();                                                // fresh driver state (route cache)
    f.h.ticks(600);
    const std::uint64_t a = f.world().state_hash();
    f.world().restore(snap);
    f.use_bot();
    f.h.ticks(600);
    CHECK(f.world().state_hash() == a);
}
