// After warm-up, views, commands, engine systems, hashing and snapshots allocate nothing.
#include "ecs_test.h"
#include <lam/ecs/commands.h>
#include <lam/ecs/lifetime.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <vector>

using namespace lam;
using namespace ecs_test;
using namespace lam::literals;

namespace {
void populate(World &w, int n) {
    for (int i = 0; i < n; ++i) {
        const Entity e = w.create();
        w.add<Transform>(e, Transform{{float(i), 0}});
        w.add<Vel>(e, Vel{{1, 1}});
        if (i % 4 == 0) w.add<Frozen>(e);
    }
    const Entity root = w.create();
    w.add<Transform>(root);
    for (int i = 0; i < 8; ++i)
        set_parent(w, w.create(), root, Transform{{float(i), 0}});
}

// One simulated tick: systems, balanced churn through commands, engine bookkeeping. The
// population is constant (every destroyed mover is replaced; about 3 short-lived effects).
void tick(World &w) {
    w.advance_tick();
    copy_prev_transforms(w);
    w.view<Transform, Vel>(Exclude<Frozen>{}).each([](Entity, Transform &t, Vel &v) {
        t.position += v.v;
    });
    int k = 0, destroyed = 0;
    for (auto [e, t, v] : w.view<Transform, Vel>()) {
        (void)t, (void)v;
        if (++k % 10 == 0) {
            w.commands().destroy(e);
            ++destroyed;
        }
    }
    for (int i = 0; i < destroyed; ++i) {
        const Entity e = w.commands().spawn();
        w.commands().add(e, Transform{{w.rng("spawn"_id).range(0, 100), 0}});
        w.commands().add(e, Vel{{0, 1}});
    }
    const Entity fx = w.commands().spawn(); // short-lived effect
    w.commands().add(fx, Transform{{1, 1}});
    w.commands().add(fx, Lifetime{3});
    w.commands().apply();
    propagate_parents(w);
    tick_lifetimes(w);
    w.commands().apply();
    settle_spawned(w);
    (void)w.state_hash();
}
} // namespace

TEST_CASE("no allocation in steady-state view iteration") {
    World w;
    populate(w, 2000);
    float sum = 0;
    auto iterate = [&] {
        w.view<Transform, Vel>().each(
            [&](Entity, Transform &t, Vel &v) { sum += t.position.x + v.v.x; });
        for (auto [e, t] : w.view<Transform>(With<Vel>{}, Exclude<Frozen>{}))
            sum += t.position.y + float(e.index);
    };
    iterate();
    CHECK_NO_ALLOC(iterate());
    CHECK(sum != 0);
}

TEST_CASE("no allocation in command apply, churn and engine systems after warm-up") {
    World w;
    populate(w, 2000);
    for (int i = 0; i < 30; ++i)
        tick(w);
    CHECK_NO_ALLOC(for (int i = 0; i < 60; ++i) tick(w));
    CHECK(w.entity_count() > 1000);
}

TEST_CASE("no allocation in snapshot_into/restore and History after warm-up") {
    World w;
    populate(w, 1000);
    w.resources().insert<Score>();
    w.snapshot_resource<Score>();
    WorldSnapshot s;
    for (int i = 0; i < 3; ++i) {
        tick(w);
        w.snapshot_into(s);
        tick(w);
        w.restore(s);
    }
    CHECK_NO_ALLOC(for (int i = 0; i < 5; ++i) {
        tick(w);
        w.snapshot_into(s);
        tick(w);
        w.restore(s);
    });
    History h(std::size_t(64) << 20, 4);
    for (int i = 0; i < 8; ++i) {
        h.push(w);
        tick(w);
    }
    h.undo(w);
    h.redo(w);
    CHECK_NO_ALLOC(for (int i = 0; i < 8; ++i) {
        h.push(w);
        tick(w);
        h.undo(w);
        h.redo(w);
    });
}

TEST_CASE("CHECK_NO_ALLOC canary: growing a pool past its capacity allocates") {
    World w;
    const Entity e = w.create();
    w.add<Health>(e);
    std::vector<Entity> es;
    es.reserve(5000);
    for (int i = 0; i < 5000; ++i)
        es.push_back(w.create());
    if (!allocation_counting_enabled()) return; // asan builds: nothing to canary
    MUST_FAIL(CHECK_NO_ALLOC(for (const Entity x : es) w.add<Health>(x)));
}
