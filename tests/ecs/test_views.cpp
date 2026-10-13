// Views: matching, With/Exclude, smallest-pool driving, dense order, tags, and the iteration lock.
#include "ecs_test.h"
#include <algorithm>
#include <lam/ecs/commands.h>
#include <lam/ecs/world.h>
#include <vector>

using namespace lam;
using namespace ecs_test;

namespace {
struct Fixture {
    World w;
    std::vector<Entity> es;
    Fixture() {
        for (int i = 0; i < 10; ++i) {
            const Entity e = w.create();
            es.push_back(e);
            w.add<Pos>(e, Pos{{float(i), 0}});
            if (i % 2 == 0) w.add<Vel>(e, Vel{{1, float(i)}});
            if (i % 3 == 0) w.add<Frozen>(e);
        }
    }
};
std::vector<std::uint32_t> indices_of(std::vector<Entity> v) {
    std::vector<std::uint32_t> out;
    for (const Entity e : v)
        out.push_back(e.index);
    return out;
}
} // namespace

TEST_CASE("view yields entities that have every component, with references") {
    Fixture f;
    std::vector<Entity> seen;
    for (auto [e, p, v] : f.w.view<Pos, Vel>()) {
        seen.push_back(e);
        CHECK(p.v.x == float(e.index));
        CHECK(v.v.y == float(e.index));
        p.v.y = 5; // writes go to the pool
    }
    CHECK(indices_of(seen) == std::vector<std::uint32_t>{0, 2, 4, 6, 8});
    CHECK(f.w.get<Pos>(f.es[4]).v.y == 5);
    CHECK(f.w.get<Pos>(f.es[3]).v.y == 0);
}

TEST_CASE("each() matches range-for, and the smallest pool drives in its dense order") {
    Fixture f;
    // Make Vel's dense order differ from Pos's: remove and re-add entity 0's Vel.
    f.w.remove<Vel>(f.es[0]);
    f.w.add<Vel>(f.es[0], Vel{{1, 0}});
    std::vector<Entity> a, b;
    f.w.view<Pos, Vel>().each([&](Entity e, Pos &, Vel &) { a.push_back(e); });
    for (auto [e, p, v] : f.w.view<Pos, Vel>())
        b.push_back(e);
    CHECK(a == b);
    // Vel (5 entities) drives: after the swap-remove its dense order is 8 2 4 6 0.
    CHECK(indices_of(a) == std::vector<std::uint32_t>{8, 2, 4, 6, 0});
    CHECK(f.w.view<Pos, Vel>().size_hint() == 5);
    CHECK(f.w.view<Pos>().size_hint() == 10);
}

TEST_CASE("With requires without yielding; Exclude filters") {
    Fixture f;
    std::vector<Entity> with, without, both;
    f.w.view<Pos>(With<Frozen>{}).each([&](Entity e, Pos &) { with.push_back(e); });
    f.w.view<Pos>(Exclude<Frozen>{}).each([&](Entity e, Pos &) { without.push_back(e); });
    f.w.view<Pos>(With<Vel>{}, Exclude<Frozen>{}).each([&](Entity e, Pos &) { both.push_back(e); });
    CHECK(indices_of(with) == std::vector<std::uint32_t>{0, 3, 6, 9});
    CHECK(indices_of(without) == std::vector<std::uint32_t>{1, 2, 4, 5, 7, 8});
    CHECK(indices_of(both) == std::vector<std::uint32_t>{2, 4, 8});
    std::size_t n = 0;
    for (auto [e, p] : f.w.view<Pos>(Exclude<Frozen, Vel>{})) {
        (void)p;
        CHECK(!f.w.has<Frozen>(e));
        CHECK(!f.w.has<Vel>(e));
        ++n;
    }
    CHECK(n == 3); // 1 5 7
}

TEST_CASE("views over missing pools are empty; excluding a missing pool filters nothing") {
    World w;
    const Entity e = w.create();
    w.add<Pos>(e);
    std::size_t n = 0;
    for (auto [x, p, h] : w.view<Pos, Health>()) {
        (void)x, (void)p, (void)h;
        ++n;
    }
    CHECK(n == 0);
    w.view<Pos>(With<Health>{}).each([&](Entity, Pos &) { ++n; });
    CHECK(n == 0);
    w.view<Pos>(Exclude<Health>{}).each([&](Entity, Pos &) { ++n; });
    CHECK(n == 1);
    CHECK(w.view<Health>().size_hint() == 0);
    CHECK(w.view<Health>().begin() == w.view<Health>().end());
}

TEST_CASE("tags can be yielded") {
    Fixture f;
    std::size_t n = 0;
    f.w.view<Frozen, Pos>().each([&](Entity, Frozen &, Pos &) { ++n; });
    CHECK(n == 4);
}

TEST_CASE("a tag alone can be viewed; With<Tag> filters the same entities") {
    Fixture f;
    std::vector<Entity> alone, with;
    for (auto [e, tag] : f.w.view<Frozen>()) {
        (void)tag;
        alone.push_back(e);
    }
    f.w.view<Pos>(With<Frozen>{}).each([&](Entity e, Pos &) { with.push_back(e); });
    CHECK(indices_of(alone) == std::vector<std::uint32_t>{0, 3, 6, 9});
    std::sort(with.begin(), with.end(), [](Entity a, Entity b) { return a.index < b.index; });
    CHECK(indices_of(with) == indices_of(alone));
}

TEST_CASE("iterating() is true only while a view is live") {
    Fixture f;
    CHECK(!f.w.iterating());
    {
        auto v = f.w.view<Pos>();
        CHECK(!f.w.iterating()); // not locked until begin()
        for (auto [e, p] : v) {
            (void)e, (void)p;
            CHECK(f.w.iterating());
            break;
        }
        CHECK(f.w.iterating()); // still alive: still locked
    }
    CHECK(!f.w.iterating());
    f.w.view<Pos>().each([&](Entity, Pos &) { CHECK(f.w.iterating()); });
    CHECK(!f.w.iterating());
}

TEST_CASE("allowed during iteration: value writes, create, structural changes to other pools") {
    Fixture f;
    f.w.view<Pos>(Exclude<Frozen>{}).each([&](Entity e, Pos &p) {
        p.v.x += 100;
        f.w.add<Health>(e); // Health is not part of the view
        (void)f.w.create();
    });
    CHECK(f.w.count<Health>() == 6);
    CHECK(f.w.entity_count() == 16);
    CHECK(f.w.get<Pos>(f.es[1]).v.x == 101);
}

// LAM_CHECK aborts in every build; these run in a forked child.
TEST_CASE("structural changes to an iterated pool fail LAM_CHECK") {
    CHECK(aborts([] {
        Fixture f;
        for (auto [e, p] : f.w.view<Pos>()) {
            (void)p;
            f.w.add<Pos>(f.w.create()); // insert into the iterated pool
        }
    }));
    CHECK(aborts([] {
        Fixture f;
        f.w.view<Pos, Vel>().each([&](Entity e, Pos &, Vel &) { f.w.remove<Vel>(e); });
    }));
    CHECK(aborts([] {
        Fixture f;
        f.w.view<Pos>(Exclude<Frozen>{}).each([&](Entity e, Pos &) {
            f.w.add<Frozen>(e);
        }); // excluded pools lock too
    }));
    CHECK(aborts([] {
        Fixture f;
        f.w.view<Pos>().each(
            [&](Entity e, Pos &) { f.w.destroy(e); }); // destroy: never while iterating
    }));
    CHECK(aborts([] {
        Fixture f;
        f.w.view<Pos>().each([&](Entity e, Pos &) {
            f.w.commands().destroy(e);
            f.w.commands().apply(); // apply is never allowed inside a view
        });
    }));
    CHECK(!aborts([] { // control: the deferred path is fine
        Fixture f;
        f.w.view<Pos>().each([&](Entity e, Pos &) { f.w.commands().destroy(e); });
        f.w.commands().apply();
    }));
}

TEST_CASE("a moved view keeps the lock") {
    Fixture f;
    auto v = f.w.view<Pos>();
    auto it = v.begin();
    CHECK(f.w.iterating());
    View<Pos> moved(std::move(v));
    CHECK(f.w.iterating());
    (void)it;
}
