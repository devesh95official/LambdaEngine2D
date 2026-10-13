// Deferred commands: record order, reserved spawns, stale-generation drops, JustSpawned, arena.
#include "ecs_test.h"
#include <lam/ecs/commands.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <string>
#include <vector>

using namespace lam;
using namespace ecs_test;

namespace {
// A non-trivially-copyable component: payloads must be constructed and destroyed properly.
struct Label {
    std::string text;
};
} // namespace

TEST_CASE("spawn reserves the index now; components appear at apply with JustSpawned") {
    World w;
    Commands &c = w.commands();
    const Entity e = c.spawn();
    CHECK(!e.is_null());
    CHECK(!w.alive(e)); // reserved, not alive yet
    const Entity other = w.create();
    CHECK(other.index != e.index); // the index is taken
    c.add(e, Health{4, 8});
    c.add(e, Frozen{});
    CHECK(c.pending() == 3);
    CHECK(c.bytes_used() > 0);
    c.apply();
    CHECK(c.pending() == 0);
    CHECK(c.bytes_used() == 0);
    REQUIRE(w.alive(e));
    CHECK(w.get<Health>(e).hp == 4);
    CHECK(w.has<Frozen>(e));
    CHECK(w.has<JustSpawned>(e));
    CHECK(!w.has<JustSpawned>(other));
}

TEST_CASE("commands apply in record order") {
    World w;
    const Entity e = w.create();
    Commands &c = w.commands();
    c.add(e, Health{1, 1});
    c.add(e, Health{2, 2});
    c.remove<Health>(e);
    c.add(e, Health{3, 3});
    c.apply();
    CHECK(w.get<Health>(e).hp == 3);
    c.remove<Health>(e);
    c.add(e, Health{9, 9});
    c.apply();
    CHECK(w.get<Health>(e).hp == 9);
}

TEST_CASE("commands for an entity whose generation no longer matches are dropped") {
    World w;
    Commands &c = w.commands();
    const Entity old = w.create();
    w.destroy(old);
    const Entity reused = w.create(); // same index, new generation
    REQUIRE(reused.index == old.index);
    c.add(old, Health{1, 1});
    c.teleport(old, {5, 5});
    c.remove<Pos>(old);
    c.destroy(old);
    c.add(old, Label{"stale"});
    c.apply();
    CHECK(w.alive(reused));
    CHECK(!w.has<Health>(reused));
    CHECK(!w.has<Transform>(reused));
    CHECK(!w.has<Label>(reused));
    // Destroy recorded before a later add of the same handle: the add is dropped.
    const Entity e = w.create();
    c.destroy(e);
    c.add(e, Health{2, 2});
    c.apply();
    CHECK(!w.alive(e));
    CHECK(w.count<Health>() == 0);
}

TEST_CASE("destroy cascades to children; teleport goes through") {
    World w;
    const Entity root = w.create();
    const Entity kid = w.create();
    const Entity grandkid = w.create();
    w.add<Transform>(root);
    set_parent(w, kid, root, Transform{{1, 0}});
    set_parent(w, grandkid, kid, Transform{{1, 0}});
    Commands &c = w.commands();
    c.teleport(root, {50, 60});
    c.apply();
    CHECK(w.get<Transform>(root).position == Vec2{50, 60});
    CHECK(w.has<JustSpawned>(root));
    CHECK(w.has<JustSpawned>(grandkid));
    c.destroy(root);
    c.apply();
    CHECK(!w.alive(root));
    CHECK(!w.alive(kid));
    CHECK(!w.alive(grandkid));
    CHECK(w.entity_count() == 0);
}

TEST_CASE("non-trivial payloads are moved into the world; discarded payloads are destroyed") {
    World w;
    Commands &c = w.commands();
    const Entity e = c.spawn();
    c.add(e, Label{std::string(100, 'x')}); // heap-backed string
    c.apply();
    REQUIRE(w.has<Label>(e));
    CHECK(w.get<Label>(e).text.size() == 100);
    c.add(e, Label{std::string(200, 'y')}); // replace path (copy-assign)
    c.apply();
    CHECK(w.get<Label>(e).text.size() == 200);
    // Pending records are discarded by clear(); payload destructors run (ASan would flag leaks).
    c.add(e, Label{std::string(300, 'z')});
    const Entity pending = c.spawn();
    w.clear();
    CHECK(c.pending() == 0);
    CHECK(!w.alive(pending));
    CHECK(w.entity_count() == 0);
}

TEST_CASE("records added while applying are applied in the same call") {
    World w;
    static Entity spawned_by_hook;
    struct Marker {
        int v = 0;
    };
    ComponentRegistry::global().set_hooks<Marker>(
        [](World &world, Entity, void *) {
            spawned_by_hook = world.commands().spawn();
            world.commands().add(spawned_by_hook, Health{7, 7});
        },
        nullptr, nullptr);
    const Entity e = w.create();
    w.commands().add(e, Marker{1});
    w.commands().apply();
    CHECK(w.commands().pending() == 0);
    REQUIRE(w.alive(spawned_by_hook));
    CHECK(w.get<Health>(spawned_by_hook).hp == 7);
    ComponentRegistry::global().set_hooks<Marker>(nullptr, nullptr, nullptr);
}

TEST_CASE("a full arena chains a new chunk and keeps order") {
    WorldConfig cfg;
    cfg.commands_arena_bytes = 1; // clamped to a small minimum
    World w(cfg);
    test::CaptureLog log;
    Commands &c = w.commands();
    std::vector<Entity> es;
    for (int i = 0; i < 500; ++i) {
        es.push_back(c.spawn());
        c.add(es.back(), Health{i, i});
    }
    c.apply();
    for (int i = 0; i < 500; ++i)
        CHECK(w.get<Health>(es[i]).hp == i);
    if (LAM_DEV) CHECK(log.contains("arena"));
    // Steady state after growth: no further allocation.
    for (int round = 0; round < 3; ++round) {
        for (const Entity e : es)
            c.add(e, Health{round, round});
        c.apply();
    }
    CHECK_NO_ALLOC(for (const Entity e : es) c.add(e, Health{5, 5}); c.apply());
    CHECK(w.get<Health>(es[499]).hp == 5);
}

TEST_CASE("snapshots taken with pending spawns record the reserved index as free") {
    World w;
    const Entity e = w.commands().spawn();
    WorldSnapshot s;
    w.snapshot_into(s);
    w.commands().apply();
    CHECK(w.alive(e));
    w.restore(s);
    CHECK(!w.alive(e));
    CHECK(w.commands().pending() == 0);
    const Entity f = w.create();
    CHECK(f.index == e.index);
    CHECK(f.generation == e.generation + 1);
}
