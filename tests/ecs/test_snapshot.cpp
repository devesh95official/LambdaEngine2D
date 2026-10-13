// Snapshots, restore, state_hash and History.
#include "ecs_test.h"
#include <lam/ecs/commands.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <vector>

using namespace lam;
using namespace ecs_test;
using namespace lam::literals;

namespace {
// A deterministic "game step": RNG-driven spawns, moves, removals and destroys.
void step(World &w) {
    Rng &r = w.rng("sim"_id);
    w.advance_tick();
    std::vector<Entity> doomed;
    w.view<Pos, Vel>().each([&](Entity e, Pos &p, Vel &v) {
        p.v += v.v;
        if (r.below(10) == 0) doomed.push_back(e);
    });
    for (const Entity e : doomed)
        w.commands().destroy(e);
    for (int i = 0; i < 3; ++i) {
        const Entity e = w.commands().spawn();
        w.commands().add(e, Pos{{r.range(0, 100), r.range(0, 100)}});
        w.commands().add(e, Vel{{r.range(-1, 1), r.range(-1, 1)}});
        if (r.chance(0.3f)) w.commands().add(e, Frozen{});
        if (r.chance(0.5f)) w.commands().add(e, Health{r.between(1, 9), 9});
    }
    w.commands().apply();
    settle_spawned(w);
    w.resources().get<Score>().moves += 1;
}

void build(World &w) {
    w.resources().insert<Score>(Score{0, 0});
    w.snapshot_resource<Score>();
    for (int i = 0; i < 20; ++i) {
        const Entity e = w.create();
        w.add<Pos>(e, Pos{{float(i), 0}});
        w.add<Vel>(e, Vel{{1, 0.5f}});
        w.add<Transform>(e, Transform{{float(i), 1}});
        w.add<RuntimeHandle>(e, RuntimeHandle{std::uint32_t(i + 100)});
    }
    const Entity root = w.create();
    w.add<Transform>(root, Transform{{5, 5}});
    w.add<Name>(root, Name{"root"_id});
    for (int i = 0; i < 3; ++i)
        set_parent(w, w.create(), root, Transform{{float(i), 0}});
}
} // namespace

TEST_CASE("snapshot/restore round trip is exact (hash, entity table, dense order)") {
    World w;
    build(w);
    for (int i = 0; i < 5; ++i)
        step(w);
    const std::uint64_t h0 = w.state_hash();
    WorldSnapshot s;
    CHECK(s.empty());
    w.snapshot_into(s);
    CHECK(!s.empty());
    CHECK(s.tick() == 5);
    CHECK(s.bytes() > 0);
    // Continue, remember the result, rewind, continue again: identical.
    for (int i = 0; i < 10; ++i)
        step(w);
    const std::uint64_t h1 = w.state_hash();
    const Entity next1 = w.create();
    CHECK(h1 != h0);
    w.restore(s);
    CHECK(w.tick() == 5);
    CHECK(w.state_hash() == h0);
    for (int i = 0; i < 10; ++i)
        step(w);
    CHECK(w.state_hash() == h1);
    CHECK(w.create() == next1); // free list restored exactly
}

TEST_CASE("restore prunes runtime pools of dead entities and keeps live ones") {
    World w;
    build(w);
    const Entity keep = std::get<0>(*w.view<RuntimeHandle>().begin());
    WorldSnapshot s;
    w.snapshot_into(s);
    const Entity late = w.create();
    w.add<RuntimeHandle>(late, RuntimeHandle{7});
    w.add<Pos>(late);
    w.get<RuntimeHandle>(keep).handle = 999;
    w.restore(s);
    CHECK(!w.alive(late));
    CHECK(w.count<RuntimeHandle>() == 20);
    CHECK(w.get<RuntimeHandle>(keep).handle == 999); // runtime state is not snapshotted
    const Entity reuse = w.create();
    CHECK(reuse.index == late.index);
    CHECK(!w.has<RuntimeHandle>(reuse)); // no stale runtime component
    CHECK(!w.has<Pos>(reuse));
}

TEST_CASE("restore options, resources, restore hooks and WorldRestored") {
    static int hook_runs = 0;
    World w;
    build(w);
    w.add_restore_hook([](World &) { ++hook_runs; });
    w.events().add<WorldRestored>();
    w.advance_tick();
    const std::uint32_t r0 = Rng(w.rng("x"_id)).next_u32();
    WorldSnapshot s;
    w.snapshot_into(s);
    w.resources().get<Score>().points = 50;
    (void)w.rng("x"_id).next_u32();
    w.advance_tick();
    w.restore(s, RestoreOptions{.tick = false, .rng = false});
    CHECK(w.tick() == 2);                          // kept
    CHECK(Rng(w.rng("x"_id)).next_u32() != r0);    // rng kept (advanced)
    CHECK(w.resources().get<Score>().points == 0); // resource restored
    CHECK(hook_runs == 1);
    w.restore(s);
    CHECK(w.tick() == 1);
    CHECK(Rng(w.rng("x"_id)).next_u32() == r0);
    CHECK(hook_runs == 2);
    const auto evs = w.events().get<WorldRestored>().all();
    REQUIRE(evs.size() == 2);
    CHECK(evs[1].tick == 1);
}

TEST_CASE("restore rebuilds Children and PrevTransform from a save-like snapshot") {
    World w;
    build(w);
    WorldSnapshot full;
    w.snapshot_into(full);
    // A "save": only kSerialize pools and resources, as lam_scene would write them.
    WorldSnapshot save;
    save.reset(full.tick(), full.seed());
    save.set_entities(full.generations(), full.slot_states(), full.free_list());
    save.set_rng_streams(full.rng_streams());
    const ComponentRegistry &reg = ComponentRegistry::global();
    for (std::size_t i = 0; i < full.pool_count(); ++i) {
        const ComponentInfo &info = reg.at(full.pool_component(i));
        if (!(info.flags & kSerialize)) continue;
        void *dst = save.add_pool(info.id, full.pool_entities(i));
        if (info.size) {
            for (std::size_t k = 0; k < full.pool_entities(i).size(); ++k)
                info.copy(static_cast<std::byte *>(dst) + k * info.size,
                          static_cast<const std::byte *>(full.pool_data(i)) + k * info.size);
        }
    }
    for (std::size_t i = 0; i < full.resource_count(); ++i) {
        const ComponentInfo &info = reg.at(full.resource_component(i));
        info.copy(save.add_resource(info.id, full.resource_flags(i)), full.resource_data(i));
    }
    REQUIRE(save.validate().ok());
    World loaded;
    loaded.resources().insert<Score>();
    loaded.snapshot_resource<Score>();
    loaded.restore(save);
    CHECK(loaded.state_hash() == w.state_hash());
    const Entity root = find_by_name(loaded, "root"_id);
    REQUIRE(loaded.has<Children>(root));
    CHECK(loaded.get<Children>(root).count == 3);
    CHECK(loaded.get<Children>(root).first == w.get<Children>(root).first);
    CHECK(loaded.count<PrevTransform>() == loaded.count<Transform>());
    CHECK(loaded.count<RuntimeHandle>() == 0);
}

TEST_CASE("validate rejects inconsistent built snapshots") {
    WorldSnapshot s;
    CHECK(!s.validate().ok());
    s.reset(0, 1);
    const std::vector<std::uint32_t> gens{1, 1};
    const std::vector<EntitySlotState> states{EntitySlotState::Alive, EntitySlotState::Free};
    s.set_entities(gens, states, {});
    CHECK(!s.validate().ok()); // free slot missing from the free list
    const std::vector<std::uint32_t> free{1};
    s.set_entities(gens, states, free);
    CHECK(s.validate().ok());
    const std::vector<Entity> ents{Entity{1, 1}};
    (void)s.add_pool(ComponentRegistry::global().id<Health>(), ents);
    CHECK(!s.validate().ok()); // entity 1 is not alive
}

TEST_CASE("state_hash covers hashed state only and is order-sensitive") {
    World a, b;
    const Entity ea = a.create(), eb = b.create();
    a.add<Health>(ea, 1, 2);
    b.add<Health>(eb, 1, 2);
    CHECK(a.state_hash() == b.state_hash());
    a.add<RuntimeHandle>(ea, RuntimeHandle{5}); // unreflected: not hashed
    a.add<PrevTransform>(ea);                   // kSnapshot only: not hashed
    CHECK(a.state_hash() == b.state_hash());
    a.get<Health>(ea).hp = 3;
    CHECK(a.state_hash() != b.state_hash());
    a.get<Health>(ea).hp = 1;
    a.add<Pos>(ea, Pos{{-0.0f, 0}});
    b.add<Pos>(eb, Pos{{0.0f, 0}});
    CHECK(a.state_hash() == b.state_hash()); // -0 == +0
    a.remove<Pos>(ea);                       // empty pools do not count
    b.remove<Pos>(eb);
    CHECK(a.state_hash() == b.state_hash());
    a.advance_tick();
    CHECK(a.state_hash() != b.state_hash());
}

TEST_CASE("History undo/redo") {
    World w;
    build(w);
    History h;
    CHECK(!h.can_undo());
    CHECK(!h.undo(w));
    CHECK(!h.redo(w));
    const std::uint64_t h0 = w.state_hash();
    h.push(w);
    step(w);
    const std::uint64_t h1 = w.state_hash();
    h.push(w);
    step(w);
    const std::uint64_t h2 = w.state_hash();
    CHECK(h.size() == 2);
    const RestoreOptions full{}; // restore the tick too: exact hashes
    REQUIRE(h.undo(w, full));
    CHECK(w.state_hash() == h1);
    CHECK(h.can_redo());
    REQUIRE(h.undo(w, full));
    CHECK(w.state_hash() == h0);
    CHECK(!h.can_undo());
    REQUIRE(h.redo(w, full));
    CHECK(w.state_hash() == h1);
    REQUIRE(h.redo(w, full));
    CHECK(w.state_hash() == h2);
    CHECK(!h.can_redo());
    CHECK(h.size() == 2);
    // Default options keep the world tick (undo is not time travel).
    REQUIRE(h.undo(w));
    CHECK(w.tick() == 2);
    CHECK(w.resources().get<Score>().moves == 1);
    h.push(w); // push clears redo
    CHECK(!h.can_redo());
    CHECK(h.bytes() > 0);
    h.clear();
    CHECK(h.size() == 0);
    CHECK(h.bytes() == 0);
}

TEST_CASE("History is bounded by entries and bytes") {
    World w;
    build(w);
    WorldSnapshot probe;
    w.snapshot_into(probe);
    const std::size_t one = probe.bytes();
    History by_count(std::size_t(1) << 30, 4);
    for (int i = 0; i < 10; ++i) {
        by_count.push(w);
        w.resources().get<Score>().points = i;
    }
    CHECK(by_count.size() == 4);
    int undos = 0;
    while (by_count.undo(w))
        ++undos;
    CHECK(undos == 4);
    CHECK(w.resources().get<Score>().points == 5); // the oldest kept push
    History by_bytes(one * 3 + one / 2, 256);
    for (int i = 0; i < 10; ++i)
        by_bytes.push(w);
    CHECK(by_bytes.size() == 3);
    CHECK(by_bytes.bytes() <= one * 3 + one / 2);
}

namespace {
int g_rebuilds = 0;
void rebuild_cache(World &) { ++g_rebuilds; }
} // namespace

TEST_CASE("has_restore_hook makes game hooks idempotent; duplicates run twice") {
    g_rebuilds = 0;
    World w;
    CHECK_FALSE(w.has_restore_hook(&rebuild_cache));
    CHECK(w.has_restore_hook(&restore_engine_state)); // the World constructor installs it first
    auto add_once = [](World &world) {
        if (!world.has_restore_hook(&rebuild_cache)) world.add_restore_hook(&rebuild_cache);
    };
    add_once(w);
    add_once(w); // a second init path: still one hook
    CHECK(w.has_restore_hook(&rebuild_cache));
    WorldSnapshot s;
    w.snapshot_into(s);
    w.restore(s);
    CHECK(g_rebuilds == 1);
    w.add_restore_hook(&rebuild_cache); // add_restore_hook itself is not idempotent
    w.restore(s);
    CHECK(g_rebuilds == 3);
}

TEST_CASE("frame-stage rule: unhashed components never change state_hash; new entities do") {
    World w;
    const Entity e = w.create();
    w.add<Health>(e, 3, 3);
    const std::uint64_t h = w.state_hash();
    w.add<RuntimeHandle>(e, RuntimeHandle{1}); // presentation-like: not hashed
    w.add<PrevTransform>(e);                   // kSnapshot only
    CHECK(w.state_hash() == h);
    w.remove<RuntimeHandle>(e);
    CHECK(w.state_hash() == h);
    const Entity fx = w.create(); // an entity created in a frame stage changes the hash...
    CHECK(w.state_hash() != h);
    w.destroy(fx);
    CHECK(w.state_hash() == h);
    // ...and even destroyed again it shifts later handles: the next simulation entity reuses
    // fx's index with a new generation, so the run diverges from one that never created fx.
    World clean;
    clean.add<Health>(clean.create(), 3, 3);
    REQUIRE(clean.state_hash() == h);
    w.add<Health>(w.create(), 1, 1);
    clean.add<Health>(clean.create(), 1, 1);
    CHECK(w.state_hash() != clean.state_hash());
}
