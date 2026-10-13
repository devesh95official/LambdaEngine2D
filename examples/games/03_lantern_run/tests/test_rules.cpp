// Lantern Run rules, headless. The first group needs only the ECS and the sim plugin; the bite,
// game-over and save cases run on the full headless engine (physics motors, steering, scene).
#include <doctest/doctest.h>
#include <lam/core/data.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/physics/motors.h>
#include <lam/physics/tile_world.h>
#include <lam/scene/save.h>

#include "forest_fixture.h"
#include "sim/level.h"
#include "sim/rules.h"

using namespace lantern;
using lantern::test::Forest;

TEST_CASE("the forest parses: size, start, dens, trees, rocks") {
    lam::Result<Level> level = parse_level(test::forest_text());
    REQUIRE(level);
    CHECK(level->width == 48);
    CHECK(level->height == 30);
    CHECK(level->start == lam::IVec2{4, 2});
    CHECK(level->dens.size() == 4);
    CHECK(level->trees.size() == 57);
    CHECK(level->rocks.size() == 13);
    CHECK(level->solid_at({0, 0}));                            // edge
    CHECK(level->solid_at({15, 1}));                           // the first tree
    CHECK_FALSE(level->solid_at({4, 2}));                      // the start
    CHECK(level->solid_at({-1, 5}));                           // outside
    CHECK(tile_feet({4, 2}) == lam::Vec2{72.0f, 47.0f});
}

TEST_CASE("facing: the larger axis wins and a diagonal keeps the current facing") {
    CHECK(facing_for({1, 0.5f}, Facing::Down) == Facing::Right);
    CHECK(facing_for({0.2f, -1}, Facing::Left) == Facing::Up);
    CHECK(facing_for({1, 1}, Facing::Down) == Facing::Down);
    CHECK(facing_for({1, 1}, Facing::Up) == Facing::Right);
    CHECK(facing_for({}, Facing::Left) == Facing::Left);
    CHECK(clamp_input({3, 4}) == lam::Vec2{0.6f, 0.8f});
}

TEST_CASE("a run starts with the full cast: 1 traveller, 3 wolves, 4 lanterns, forest scenery") {
    Forest f;
    f.h.ticks(1);
    lam::World &w = f.world();
    CHECK(w.count<Player>() == 1);
    CHECK(w.count<Wolf>() == 3);
    CHECK(w.count<Lantern>() == 4);
    CHECK(w.count<Tree>() == 57);
    CHECK(w.count<Rock>() == 13);
    CHECK(f.run().phase == Phase::Playing);
    CHECK(f.run().runs == 1);
    const Level &level = w.resources().get<Level>();
    const lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    CHECK(tiles.at({15, 1}) == lam::TileShape::Solid);
    CHECK(tiles.opaque({15, 1}));
    CHECK(tiles.cost(15, 1) == 0);
    const lam::Vec2 start = tile_feet(level.start);
    for (auto [e, l, tr] : w.view<Lantern, lam::Transform>()) {
        (void)e;
        CHECK(l.present);
        CHECK(lam::distance(tr.position, start) >= 100.0f);
        CHECK_FALSE(level.solid_at(tiles.tile_at(tr.position)));
    }
}

TEST_CASE("the title waits for confirm: nothing ticks the rules") {
    Forest f(777, false, false);
    f.h.ticks(30);
    CHECK(f.run().phase == Phase::Title);
    CHECK(f.world().get<Player>(f.player()).dash_cooldown == 0);
}

TEST_CASE("every 8th lantern starts a new wave and another wolf joins") {
    Forest f;
    f.h.ticks(1);
    lam::World &w = f.world();
    for (int i = 0; i < 8; ++i) {
        // Light the first lantern right under the traveller's feet; PostPhysics collects it.
        const lam::Vec2 feet = w.get<lam::Transform>(f.player()).position;
        for (auto [e, l] : w.view<Lantern>()) {
            if (!l.present) continue;
            w.get<lam::Transform>(e).position = feet;
            break;
        }
        f.h.ticks(1);
    }
    CHECK(f.run().collected == 8);
    CHECK(f.run().best == 8);
    CHECK(f.run().wave == 2);
    CHECK(w.count<Wolf>() == 4);
    CHECK(w.count<Lantern>() == 4);
}

TEST_CASE("Fsm timings: telegraph 26, lunge 12, recover 36, then cooldown 40") {
    Forest f;
    f.h.ticks(1);
    lam::World &w = f.world();
    lam::Entity wolf_e;
    for (auto [e, wolf] : w.view<Wolf>()) {
        if (wolf.slot == 0) wolf_e = e;
    }
    REQUIRE(wolf_e);
    Wolf &wolf = w.get<Wolf>(wolf_e);
    wolf.fsm.state = WolfState::Telegraph;                      // as if Chase -> Telegraph just ran
    wolf.fsm.ticks_in_state = 1;
    f.h.ticks(25);
    CHECK(w.get<Wolf>(wolf_e).fsm.in(WolfState::Telegraph));
    f.h.ticks(1);
    CHECK(w.get<Wolf>(wolf_e).fsm.in(WolfState::Lunge));
    f.h.ticks(11);
    CHECK(w.get<Wolf>(wolf_e).fsm.in(WolfState::Lunge));
    f.h.ticks(1);
    CHECK(w.get<Wolf>(wolf_e).fsm.in(WolfState::Recover));
    f.h.ticks(35);
    CHECK(w.get<Wolf>(wolf_e).fsm.in(WolfState::Recover));
    f.h.ticks(1);
    CHECK(w.get<Wolf>(wolf_e).fsm.in(WolfState::Chase));
    CHECK(w.get<Wolf>(wolf_e).cooldown == 40);
}

TEST_CASE("undo: a restored snapshot replays bit-identically (rules state, RNG, RunState)") {
    Forest f;
    f.h.ticks(30);
    lam::WorldSnapshot snap;
    f.world().snapshot_into(snap);
    f.h.ticks(240);
    const std::uint64_t a = f.world().state_hash();
    f.world().restore(snap);
    f.h.ticks(240);
    CHECK(f.world().state_hash() == a);
}

TEST_CASE("same seed, same forest; another seed, another forest") {
    Forest a(5), b(5), c(6);
    a.h.ticks(120);
    b.h.ticks(120);
    c.h.ticks(120);
    CHECK(a.world().state_hash() == b.world().state_hash());
    CHECK(a.world().state_hash() != c.world().state_hash());
}

TEST_CASE("a lunging wolf bites once: a heart, knockback, protection") {
    Forest f(777, true);
    f.h.ticks(2);
    lam::World &w = f.world();
    const lam::Vec2 feet = w.get<lam::Transform>(f.player()).position;
    lam::Entity wolf_e;
    for (auto [e, wolf] : w.view<Wolf>()) {
        if (wolf.slot == 0) wolf_e = e;
    }
    w.teleport(wolf_e, feet - lam::Vec2{14, 0});
    Wolf &wolf = w.get<Wolf>(wolf_e);
    wolf.fsm.state = WolfState::Lunge;
    wolf.fsm.ticks_in_state = 1;
    wolf.dir = {1, 0};
    f.h.ticks(3);
    const Player &p = w.get<Player>(f.player());
    CHECK(p.hearts == 2);
    CHECK(p.protection > 0);
    CHECK(w.get<Wolf>(wolf_e).hit_this_lunge);
}

TEST_CASE("the third bite ends the run and every body stops on the game-over screen") {
    Forest f(777, true);
    f.h.ticks(2);
    lam::World &w = f.world();
    const lam::Entity pe = f.player();
    w.get<Player>(pe).hearts = 1;
    w.get<lam::TopDownMotor>(pe).move = {0, 1};                 // walking when the bite lands
    const lam::Vec2 feet = w.get<lam::Transform>(pe).position;
    lam::Entity wolf_e;
    for (auto [e, wolf] : w.view<Wolf>()) {
        if (wolf.slot == 0) wolf_e = e;
    }
    w.teleport(wolf_e, feet - lam::Vec2{14, 0});
    Wolf &wolf = w.get<Wolf>(wolf_e);
    wolf.fsm.state = WolfState::Lunge;
    wolf.fsm.ticks_in_state = 1;
    wolf.dir = {1, 0};
    f.h.ticks(3);
    REQUIRE(f.run().phase == Phase::Over);
    CHECK(f.run().deaths == 1);
    const lam::Vec2 player_at = w.get<lam::Transform>(pe).position;
    const lam::Vec2 wolf_at = w.get<lam::Transform>(wolf_e).position;
    f.h.ticks(30);
    CHECK(w.get<lam::Transform>(pe).position == player_at);
    CHECK(w.get<lam::Transform>(wolf_e).position == wolf_at);
}

TEST_CASE("save round trip: serialize_world -> deserialize_world continues bit-identically") {
    Forest f(777, true);
    f.h.ticks(60);
    lam::DataWriter out;
    REQUIRE(lam::serialize_world(f.world(), out, f.h.app().serialize_context()));
    lam::Result<std::string> text = out.finish();
    REQUIRE(text);
    f.h.ticks(120);
    const std::uint64_t a = f.world().state_hash();

    Forest g(777, true);
    g.h.ticks(1);
    lam::Result<lam::DataDocument> doc = lam::DataDocument::parse(*text, "save.json");
    REQUIRE(doc);
    lam::DataReader root = doc->root();
    REQUIRE(lam::deserialize_world(g.world(), root, g.h.app().serialize_context()));
    g.h.ticks(120);
    CHECK(g.world().state_hash() == a);
}
