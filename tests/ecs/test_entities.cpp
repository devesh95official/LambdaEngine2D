// Entities, generations, components, pool operations, RNG streams.
#include "ecs_test.h"
#include <array>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <vector>

using namespace lam;
using namespace ecs_test;
using namespace lam::literals;

TEST_CASE("entity handles: null, bits, generations start at 1") {
    CHECK(Entity{}.is_null());
    CHECK(!Entity::null());
    CHECK(Entity{3, 2}.bits() == (3ull | (2ull << 32)));
    World w;
    const Entity a = w.create();
    CHECK(a.index == 0);
    CHECK(a.generation == 1);
    CHECK(w.alive(a));
    CHECK(!w.alive(Entity{}));
    CHECK(!w.alive(Entity{0, 2}));
    CHECK(!w.alive(Entity{57, 1}));
    CHECK(w.entity_count() == 1);
}

TEST_CASE("destroyed indices are reused with generation + 1; stale handles stay dead") {
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    w.add<Health>(a);
    w.destroy(a);
    CHECK(!w.alive(a));
    CHECK(w.entity_count() == 1);
    CHECK(!w.has<Health>(a));
    const Entity c = w.create();
    CHECK(c.index == a.index);
    CHECK(c.generation == a.generation + 1);
    CHECK(!w.has<Health>(c)); // no stale component on the recycled index
    CHECK(w.try_get<Health>(a) == nullptr);
    CHECK(w.alive(b));
    w.destroy(a); // stale destroy is a no-op
    CHECK(w.alive(c));
}

TEST_CASE("a slot that reaches the last generation is retired, never reused") {
    World w;
    const Entity a = w.create();
    WorldSnapshot s;
    w.snapshot_into(s);
    // Rebuild the table with slot 0 one step from the end (engine-access builder API).
    WorldSnapshot t;
    t.reset(s.tick(), s.seed());
    const std::array<std::uint32_t, 1> gens{0xFFFFFFFEu};
    const std::array<EntitySlotState, 1> states{EntitySlotState::Alive};
    t.set_entities(gens, states, {});
    REQUIRE(t.validate().ok());
    w.restore(t);
    const Entity last{0, 0xFFFFFFFEu};
    CHECK(w.alive(last));
    CHECK(!w.alive(a));
    w.destroy(last);
    const Entity fresh = w.create();
    CHECK(fresh.index == 1); // index 0 is retired
    CHECK(fresh.generation == 1);
}

TEST_CASE("world ids are unique and never reused") {
    std::uint32_t first = 0;
    {
        World a;
        first = a.id();
        CHECK(first != 0);
    }
    World b;
    World c;
    CHECK(b.id() != first);
    CHECK(c.id() != b.id());
    WorldConfig cfg;
    cfg.name = "level"_id;
    World named(cfg);
    CHECK(named.name() == "level"_id);
}

TEST_CASE("add/get/try_get/has/remove/count and assignment on existing") {
    World w;
    const Entity e = w.create();
    Health &h = w.add<Health>(e, 3, 7); // aggregate init from args
    CHECK(h.hp == 3);
    CHECK(h.max == 7);
    CHECK(w.has<Health>(e));
    CHECK(w.get<Health>(e).max == 7);
    w.add<Health>(e, Health{1, 2}); // existing: assigned
    CHECK(w.get<Health>(e).hp == 1);
    CHECK(w.count<Health>() == 1);
    const World &cw = w;
    CHECK(cw.try_get<Health>(e)->max == 2);
    CHECK(cw.get<Health>(e).hp == 1);
    w.remove<Health>(e);
    CHECK(!w.has<Health>(e));
    CHECK(w.count<Health>() == 0);
    w.remove<Health>(e);                 // absent: no-op
    CHECK(w.try_get<Pos>(e) == nullptr); // pool never created
    w.add<Frozen>(e);                    // tag
    CHECK(w.has<Frozen>(e));
    CHECK(w.count<Frozen>() == 1);
}

TEST_CASE("swap-remove keeps other components intact") {
    World w;
    std::vector<Entity> es;
    for (int i = 0; i < 10; ++i) {
        es.push_back(w.create());
        w.add<Health>(es.back(), i, i * 10);
    }
    w.remove<Health>(es[2]);
    w.destroy(es[5]);
    for (int i = 0; i < 10; ++i) {
        if (i == 2 || i == 5) continue;
        REQUIRE(w.has<Health>(es[i]));
        CHECK(w.get<Health>(es[i]).hp == i);
        CHECK(w.get<Health>(es[i]).max == i * 10);
    }
}

TEST_CASE("type-erased access: add_raw, get_raw, remove_raw, components_of") {
    World w;
    const Entity e = w.create();
    const ComponentId hid = ComponentRegistry::global().id<Health>();
    const ComponentId pid = ComponentRegistry::global().id<Pos>();
    auto *h = static_cast<Health *>(w.add_raw(e, hid));
    REQUIRE(h != nullptr);
    CHECK(h->hp == 10); // default-constructed
    h->hp = 4;
    CHECK(w.get<Health>(e).hp == 4);
    CHECK(w.get_raw(e, pid) == nullptr);
    w.add<Pos>(e);
    std::array<ComponentId, 8> ids{};
    const std::size_t n = w.components_of(e, ids);
    CHECK(n == 2);
    CHECK(ids[0] == std::min(hid, pid));
    CHECK(ids[1] == std::max(hid, pid));
    std::array<ComponentId, 1> small{};
    CHECK(w.components_of(e, small) == 2); // total count even if truncated
    w.remove_raw(e, hid);
    CHECK(!w.has<Health>(e));
    CHECK(w.get_raw(e, hid) == nullptr);
}

TEST_CASE("single, single_or_null") {
    World w;
    Entity out{1, 1};
    CHECK(w.single_or_null<Score>(&out) == nullptr);
    CHECK(out.is_null());
    const Entity e = w.create();
    w.add<Health>(e, 5, 5);
    CHECK(w.single<Health>() == e);
    Entity got;
    Health *h = w.single_or_null<Health>(&got);
    REQUIRE(h != nullptr);
    CHECK(got == e);
    CHECK(h->hp == 5);
}

TEST_CASE("pool_version counts structural changes only") {
    World w;
    CHECK(w.pool_version<Health>() == 0);
    const Entity e = w.create();
    w.add<Health>(e);
    const std::uint32_t v1 = w.pool_version<Health>();
    CHECK(v1 > 0);
    w.get<Health>(e).hp = 99;
    w.add<Health>(e, 1, 1); // assignment: not structural
    CHECK(w.pool_version<Health>() == v1);
    w.remove<Health>(e);
    CHECK(w.pool_version<Health>() > v1);
}

TEST_CASE("reserve pre-sizes pools") {
    World w;
    w.reserve<Health>(1000);
    std::vector<Entity> es;
    for (int i = 0; i < 1000; ++i)
        es.push_back(w.create());
    CHECK_NO_ALLOC(for (const Entity e : es) w.add<Health>(e));
    CHECK(w.count<Health>() == 1000);
}

TEST_CASE("align orders follower pools like the lead pool") {
    World w;
    std::vector<Entity> es;
    for (int i = 0; i < 8; ++i)
        es.push_back(w.create());
    for (int i = 7; i >= 0; --i)
        w.add<Health>(es[i], i, i); // Health dense: 7..0
    for (int i = 0; i < 8; ++i)
        if (i % 3 != 0) w.add<Pos>(es[i], Pos{{float(i), 0}}); // Pos dense: 1 2 4 5 7
    const auto extra = w.create();
    w.add<Pos>(extra, Pos{{99, 0}}); // not in the lead pool
    w.align<Health, Pos>();
    std::vector<std::uint32_t> order;
    w.view<Pos>().each([&](Entity e, Pos &p) {
        order.push_back(e.index);
        CHECK(p.v.x == (e == extra ? 99.0f : float(e.index))); // data moved with entities
    });
    CHECK(order == std::vector<std::uint32_t>{7, 5, 4, 2, 1, extra.index});
}

TEST_CASE("sort is stable and keeps data with entities") {
    World w;
    std::vector<Entity> es;
    const int keys[] = {3, 1, 2, 1, 3, 0, 2};
    for (int i = 0; i < 7; ++i) {
        es.push_back(w.create());
        w.add<Health>(es.back(), keys[i], i);
    }
    w.sort<Health>([](const Health &a, const Health &b) { return a.hp < b.hp; });
    std::vector<int> hp, max;
    w.view<Health>().each([&](Entity e, Health &h) {
        hp.push_back(h.hp);
        max.push_back(h.max);
        CHECK(e == es[h.max]);
    });
    CHECK(hp == std::vector<int>{0, 1, 1, 2, 2, 3, 3});
    CHECK(max == std::vector<int>{5, 1, 3, 2, 6, 0, 4}); // stable within equal keys
    for (const Entity e : es)
        CHECK(w.has<Health>(e));
    // Tags sort by entity.
    for (int i = 6; i >= 0; --i)
        w.add<Frozen>(es[i]);
    w.sort<Frozen>([](Entity a, Entity b) { return a.index < b.index; });
    std::vector<std::uint32_t> idx;
    w.view<Frozen>().each([&](Entity e, Frozen &) { idx.push_back(e.index); });
    CHECK(idx == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6});
}

TEST_CASE("rng streams are independent, derived from the seed and reseedable") {
    WorldConfig cfg;
    cfg.seed = 42;
    World a(cfg), b(cfg);
    Rng &ra = a.rng("loot"_id);
    CHECK(&ra == &a.rng("loot"_id)); // same stream object
    const std::uint32_t x = ra.next_u32();
    (void)a.rng("ai"_id).next_u32(); // another stream does not shift "loot"
    const std::uint32_t y = ra.next_u32();
    Rng &rb = b.rng("loot"_id);
    CHECK(rb.next_u32() == x);
    CHECK(rb.next_u32() == y);
    CHECK(Rng(derive_seed(42, ("loot"_id).value)).next_u32() == x);
    a.reseed(7);
    CHECK(a.seed() == 7);
    CHECK(a.rng("loot"_id).next_u32() == Rng(derive_seed(7, ("loot"_id).value)).next_u32());
}

TEST_CASE("more than 32 rng streams fails LAM_CHECK") {
    CHECK(aborts([] {
        World w;
        for (std::uint64_t i = 1; i <= 33; ++i)
            (void)w.rng(StrId{i});
    }));
    World w;
    for (std::uint64_t i = 1; i <= 32; ++i)
        (void)w.rng(StrId{i}); // exactly 32 is fine
}

TEST_CASE("tick and clear") {
    World w;
    CHECK(w.tick() == 0);
    w.advance_tick();
    w.advance_tick();
    CHECK(w.tick() == 2);
    const Entity a = w.create();
    const Entity b = w.create();
    w.add<Health>(a);
    w.add<Pos>(b);
    w.resources().insert<Score>(Score{5, 1});
    w.clear();
    CHECK(w.entity_count() == 0);
    CHECK(w.count<Health>() == 0);
    CHECK(w.count<Pos>() == 0);
    CHECK(w.resources().get<Score>().points == 5); // resources kept
    const Entity c = w.create();
    CHECK(c.index == 0); // indices reused from 0 upwards
    CHECK(!w.alive(a));
}
