// ecs_demo: a console tour of lam_ecs. Spawns movers and a small hierarchy, runs a few ticks of
// systems through views and deferred commands, then shows undo/redo with History and the state
// hash that replays and saves compare.
#include <cstdio>
#include <lam/ecs/commands.h>
#include <lam/ecs/lifetime.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>

namespace demo {
struct Drift {
    lam::Vec2 v;
}; // px per tick
struct Coins {
    std::int32_t amount = 0;
}; // world resource
} // namespace demo
LAM_REFLECT(demo::Drift, v)
LAM_REFLECT(demo::Coins, amount)

namespace {
using namespace lam;
using namespace lam::literals;

void move_system(World &w) {
    w.view<Transform, demo::Drift>().each(
        [](Entity, Transform &t, demo::Drift &v) { t.position += v.v; });
}

void tick(World &w) {
    w.advance_tick();
    copy_prev_transforms(w);
    move_system(w);
    w.commands().apply();
    propagate_parents(w);
    tick_lifetimes(w);
    w.commands().apply();
    settle_spawned(w);
}

void print_world(World &w, const char *label) {
    std::printf("-- %s: tick %llu, %zu entities, coins %d, hash %016llx\n", label,
                static_cast<unsigned long long>(w.tick()), w.entity_count(),
                w.resources().get<demo::Coins>().amount,
                static_cast<unsigned long long>(w.state_hash()));
    for (auto [e, t] : w.view<Transform>(Exclude<Parent>{})) {
        const Name *n = w.try_get<Name>(e);
        std::printf("   [%u:%u] %-8s at (%6.1f, %6.1f)\n", e.index, e.generation,
                    n ? id_display(n->id).c_str() : "-", t.position.x, t.position.y);
    }
}
} // namespace

int main() {
    World w(WorldConfig{.seed = 7});
    w.resources().insert<demo::Coins>();
    w.snapshot_resource<demo::Coins>();

    // Immediate creation.
    const Entity ship = w.create();
    w.add<Transform>(ship, Transform{{10, 10}});
    w.add<demo::Drift>(ship, demo::Drift{{2, 0}});
    w.add<Name>(ship, Name{intern("ship")});
    // A child that follows the ship (hierarchy propagation).
    const Entity turret = w.create();
    set_parent(w, turret, ship, Transform{{0, -4}});

    // Prefabs from JSON, spawned through deferred commands.
    PrefabLibrary prefabs;
    const Status st = prefabs.load_text(R"({"prefabs": {"rock": {"components": {
        "Drift": {"v": [0, 1]}, "Lifetime": {"ticks": 4}, "Name": {"id": "rock"}}}}})",
                                        "demo.json", SerializeContext{});
    if (!st) {
        std::printf("prefab error: %s\n", st.error().message.c_str());
        return 1;
    }
    w.set_prefabs(&prefabs);
    for (int i = 0; i < 3; ++i)
        w.commands().spawn_prefab("rock"_id, {40.0f + 10.0f * float(i), 0});
    w.commands().apply();
    print_world(w, "start");

    History history;
    for (int i = 0; i < 3; ++i) {
        history.push(w); // state before this "move"
        w.resources().get<demo::Coins>().amount += w.rng("loot"_id).between(1, 6);
        tick(w);
    }
    print_world(w, "after 3 ticks");
    std::printf("   turret world position (%.1f, %.1f)\n", w.get<Transform>(turret).position.x,
                w.get<Transform>(turret).position.y);

    const std::uint64_t before_undo = w.state_hash();
    history.undo(w, RestoreOptions{});
    history.undo(w, RestoreOptions{});
    print_world(w, "undo x2");
    history.redo(w, RestoreOptions{});
    history.redo(w, RestoreOptions{});
    print_world(w, "redo x2");
    std::printf("redo restores the exact state: %s\n",
                w.state_hash() == before_undo ? "yes" : "NO");

    for (int i = 0; i < 3; ++i)
        tick(w);
    print_world(w, "rocks expired");
    return w.state_hash() == 0 ? 1 : 0;
}
