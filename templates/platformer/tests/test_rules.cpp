// Rule tests with no window, map or assets: the world is built by hand and contact events are
// injected. TODO: a test per rule you add.
#include <algorithm>
#include <lam/audio/components.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/events.h>
#include <lam/physics/physics_plugin.h>
#include <lam/test/support.h>
#include "platformer/game.h"

using namespace lam;
using namespace lam::literals;

TEST_CASE("a coin counts once, plays a sound and goes away; the goal wins the level") {
    test::HeadlessApp h;
    h.add<PhysicsPlugin>();                                     // RulesPlugin requires it
    h.add<platformer::RulesPlugin>();
    h.app().add_event<TriggerEntered>();                        // idempotent: physics registers these
    h.app().add_event<TileHazardTouched>();
    World &w = h.app().main_world();
    w.resources().insert<Level>();                              // what start_level does, minus the map
    w.snapshot_resource<Level>();
    const auto spawn = [&w](Vec2 at) {
        const Entity e = w.create();
        w.add<Transform>(e, Transform{.position = at});
        return e;
    };
    const Entity hero = spawn({32, 100});
    w.add<Player>(hero);
    const Entity coin = spawn({48, 96});
    w.add<Coin>(coin);
    const Entity goal = spawn({200, 100});
    w.add<Goal>(goal);
    h.ticks(1);
    const auto touch = [&](Entity trigger) {                    // what physics reports on overlap
        w.events().get<TriggerEntered>().emit(TriggerEntered{.trigger = trigger, .other = hero});
        h.ticks(1);
    };
    const Level &lv = w.resources().get<Level>();

    touch(coin);
    CHECK(lv.coins == 1);
    CHECK_FALSE(w.alive(coin));                                 // destroyed through commands
    const auto sounds = w.events().get<PlaySound>().all();
    CHECK(std::any_of(sounds.begin(), sounds.end(), [](const PlaySound &s) { return s.group == "coin"_id; }));
    CHECK_FALSE(lv.won);

    touch(goal);
    CHECK(lv.won);
    CHECK(lv.coins == 1);
    CHECK(w.events().get<LevelFinished>().all().size() == 1);   // main.cpp shows the result screen
}
