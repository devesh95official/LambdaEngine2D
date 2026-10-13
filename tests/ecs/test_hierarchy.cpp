// Hooks, the Parent/Children hierarchy, propagation, interpolation, JustSpawned and lifetimes.
#include "ecs_test.h"
#include <lam/ecs/commands.h>
#include <lam/ecs/lifetime.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <vector>

using namespace lam;
using namespace ecs_test;
using namespace lam::literals;

namespace {
struct Counted {
    int v = 0;
};
int g_added = 0, g_replaced = 0, g_removed = 0, g_last_value = 0;

Parent make_parent(Entity p, const Transform &local = {}) {
    Parent r;
    r.entity = p;
    r.local = local;
    return r;
}

std::vector<Entity> children_of(const World &w, Entity parent) {
    std::vector<Entity> out;
    const Children *ch = w.try_get<Children>(parent);
    if (!ch) return out;
    for (Entity c = ch->first; c; c = w.get<Parent>(c).next_sibling)
        out.push_back(c);
    return out;
}
} // namespace

TEST_CASE("hooks: on_add once, on_replace on re-add, on_remove on remove and destroy") {
    ComponentRegistry::global().set_hooks<Counted>(
        [](World &, Entity, void *p) {
            ++g_added;
            g_last_value = static_cast<Counted *>(p)->v;
        },
        [](World &, Entity, void *p) {
            ++g_replaced;
            g_last_value = static_cast<Counted *>(p)->v;
        },
        [](World &, Entity, void *) { ++g_removed; });
    World w;
    const Entity a = w.create();
    const Entity b = w.create();
    w.add<Counted>(a, 1);
    CHECK(g_added == 1);
    CHECK(g_last_value == 1); // hook sees the constructed value
    w.add<Counted>(a, 2);
    CHECK(g_replaced == 1);
    CHECK(g_last_value == 2);
    w.remove<Counted>(a);
    CHECK(g_removed == 1);
    w.add<Counted>(b, 3);
    w.destroy(b);
    CHECK(g_removed == 2);
    const ComponentId id = ComponentRegistry::global().id<Counted>();
    const Entity c = w.create();
    w.add_raw(c, id, false); // hooks suppressed
    CHECK(g_added == 2);
    w.add_raw(c, id); // existing: reset + on_replace
    CHECK(g_replaced == 2);
    ComponentRegistry::global().set_hooks<Counted>(nullptr, nullptr, nullptr);
}

TEST_CASE("adding Transform adds PrevTransform equal to it") {
    World w;
    const Entity e = w.create();
    w.add<Transform>(e, Transform{{3, 4}, 0.5f, {2, 2}});
    REQUIRE(w.has<PrevTransform>(e));
    const PrevTransform &p = w.get<PrevTransform>(e);
    CHECK(p.position == Vec2{3, 4});
    CHECK(p.rotation == 0.5f);
    CHECK(p.scale == Vec2{2, 2});
}

TEST_CASE("Parent hooks maintain sorted Children lists and depth") {
    World w;
    const Entity root = w.create();
    std::vector<Entity> kids;
    for (int i = 0; i < 4; ++i)
        kids.push_back(w.create());
    // Link out of index order: lists stay sorted by index.
    for (const int i : {2, 0, 3, 1})
        w.add<Parent>(kids[i], make_parent(root));
    CHECK(children_of(w, root) == kids);
    CHECK(w.get<Children>(root).count == 4);
    for (const Entity k : kids)
        CHECK(w.get<Parent>(k).depth == 1);
    const Entity grand = w.create();
    w.add<Parent>(grand, make_parent(kids[1]));
    CHECK(w.get<Parent>(grand).depth == 2);
    // Re-parent a subtree with set_parent: depths follow.
    const Entity top = w.create();
    const Entity mid = w.create();
    w.add<Parent>(mid, make_parent(top));
    set_parent(w, root, mid, Transform{});
    CHECK(w.get<Parent>(root).depth == 2);
    CHECK(w.get<Parent>(kids[0]).depth == 3);
    CHECK(w.get<Parent>(grand).depth == 4);
    // Removing a child unlinks it; the last child removes Children.
    w.remove<Parent>(kids[2]);
    CHECK(children_of(w, root) == std::vector<Entity>{kids[0], kids[1], kids[3]});
    clear_parent(w, grand);
    CHECK(!w.has<Children>(kids[1]));
    CHECK(w.get<Children>(root).count == 3);
    // Re-adding Parent on an entity that has one (on_replace) relinks.
    w.add<Parent>(kids[3], make_parent(top));
    CHECK(children_of(w, root) == std::vector<Entity>{kids[0], kids[1]});
    CHECK(children_of(w, top) == std::vector<Entity>{kids[3], mid}); // sorted by index
    CHECK(w.get<Parent>(kids[3]).depth == 1);
}

TEST_CASE("destroy cascades through the whole subtree") {
    World w;
    const Entity root = w.create();
    const Entity a = w.create(), b = w.create(), a1 = w.create(), a2 = w.create();
    const Entity bystander = w.create();
    w.add<Parent>(a, make_parent(root));
    w.add<Parent>(b, make_parent(root));
    w.add<Parent>(a1, make_parent(a));
    w.add<Parent>(a2, make_parent(a));
    w.destroy(a);
    CHECK(!w.alive(a));
    CHECK(!w.alive(a1));
    CHECK(!w.alive(a2));
    CHECK(children_of(w, root) == std::vector<Entity>{b});
    w.destroy(root);
    CHECK(!w.alive(b));
    CHECK(w.alive(bystander));
    CHECK(w.entity_count() == 1);
    CHECK(w.count<Parent>() == 0);
    CHECK(w.count<Children>() == 0);
}

TEST_CASE("propagate_parents applies parent * local in depth order") {
    World w;
    const Entity root = w.create();
    w.add<Transform>(root, Transform{{100, 0}, 0, {2, 2}});
    // Create the grandchild link before the child link so dense order is not depth order.
    const Entity child = w.create();
    const Entity grand = w.create();
    w.add<Transform>(child);
    w.add<Transform>(grand);
    w.add<Parent>(grand, make_parent(child, Transform{{1, 0}, 0, {1, 1}}));
    w.add<Parent>(child, make_parent(root, Transform{{10, 5}, 0, {1, 1}}));
    propagate_parents(w);
    CHECK(w.get<Transform>(child).position == Vec2{120, 10});
    CHECK(w.get<Transform>(child).scale == Vec2{2, 2});
    CHECK(w.get<Transform>(grand).position == Vec2{122, 10}); // child scale 2 applies to local x
    w.get<Transform>(root).position = {0, 0};
    propagate_parents(w);
    CHECK(w.get<Transform>(grand).position == Vec2{22, 10});
}

TEST_CASE("set_parent places the child immediately") {
    World w;
    const Entity p = w.create(), c = w.create();
    w.add<Transform>(p, Transform{{10, 10}});
    set_parent(w, c, p, Transform{{5, 0}});
    REQUIRE(w.has<Transform>(c));
    CHECK(w.get<Transform>(c).position == Vec2{15, 10});
    CHECK(w.get<Parent>(c).entity == p);
}

TEST_CASE("interpolation lerps Prev -> current; JustSpawned and missing PrevTransform snap") {
    World w;
    const Entity e = w.create();
    w.add<Transform>(e, Transform{{0, 0}});
    copy_prev_transforms(w);
    w.get<Transform>(e).position = {10, 20};
    CHECK(interpolated(w, e, 0.5f).position == Vec2{5, 10});
    CHECK(interpolated(w, e, 0.0f).position == Vec2{0, 0});
    CHECK(interpolated(w, e, 1.0f).position == Vec2{10, 20});
    copy_prev_transforms(w);
    CHECK(interpolated(w, e, 0.25f).position == Vec2{10, 20});
    w.add<JustSpawned>(e);
    w.get<Transform>(e).position = {30, 20};
    CHECK(interpolated(w, e, 0.5f).position == Vec2{30, 20});
    w.remove<PrevTransform>(e);
    w.remove<JustSpawned>(e);
    CHECK(interpolated(w, e, 0.5f).position == Vec2{30, 20});
    CHECK(interpolated(w, w.create(), 0.5f).position == Vec2{0, 0});
}

TEST_CASE("teleport snaps the entity and marks its subtree; settle_spawned clears the marks") {
    World w;
    const Entity root = w.create(), kid = w.create();
    w.add<Transform>(root, Transform{{0, 0}});
    set_parent(w, kid, root, Transform{{4, 0}});
    copy_prev_transforms(w);
    w.teleport(root, {100, 100});
    CHECK(w.get<Transform>(root).position == Vec2{100, 100});
    CHECK(w.get<PrevTransform>(root).position == Vec2{100, 100});
    CHECK(w.has<JustSpawned>(root));
    CHECK(w.has<JustSpawned>(kid));
    propagate_parents(w); // child moves with the parent
    CHECK(w.get<Transform>(kid).position == Vec2{104, 100});
    CHECK(interpolated(w, kid, 0.1f).position == Vec2{104, 100}); // no glide for the child
    settle_spawned(w);
    CHECK(w.count<JustSpawned>() == 0);
    CHECK(w.get<PrevTransform>(kid).position == Vec2{104, 100});
    // Teleporting a child rewrites its local transform so propagation keeps it there.
    w.teleport(kid, {110, 90});
    propagate_parents(w);
    CHECK(w.get<Transform>(kid).position == Vec2{110, 90});
    CHECK(w.get<Parent>(kid).local.position == Vec2{10, -10});
    // Teleport adds Transform when missing.
    const Entity bare = w.create();
    w.teleport(bare, {1, 2});
    CHECK(w.get<Transform>(bare).position == Vec2{1, 2});
}

TEST_CASE("commands-spawned entities do not glide in") {
    World w;
    const Entity e = w.commands().spawn();
    w.commands().add(e, Transform{{50, 50}});
    w.commands().apply();
    REQUIRE(w.has<JustSpawned>(e));
    w.get<Transform>(e).position = {60, 50}; // moved in its first tick
    CHECK(interpolated(w, e, 0.0f).position == Vec2{60, 50});
    settle_spawned(w);
    CHECK(!w.has<JustSpawned>(e));
    CHECK(interpolated(w, e, 0.0f).position == Vec2{60, 50});
}

TEST_CASE("find_by_name") {
    World w;
    const Entity a = w.create(), b = w.create();
    w.add<Name>(a, Name{"alpha"_id});
    w.add<Name>(b, Name{"beta"_id});
    CHECK(find_by_name(w, "beta"_id) == b);
    CHECK(find_by_name(w, "gamma"_id).is_null());
}

TEST_CASE("lifetimes destroy through commands when they reach zero") {
    World w;
    const Entity e = w.create();
    const Entity kid = w.create();
    w.add<Lifetime>(e, Lifetime{3});
    w.add<Parent>(kid, make_parent(e));
    for (int i = 0; i < 2; ++i) {
        tick_lifetimes(w);
        w.commands().apply();
        CHECK(w.alive(e));
    }
    CHECK(w.get<Lifetime>(e).ticks == 1);
    tick_lifetimes(w);
    CHECK(w.alive(e)); // deferred until apply
    w.commands().apply();
    CHECK(!w.alive(e));
    CHECK(!w.alive(kid));
}

TEST_CASE("register_engine_components is idempotent and sets JustSpawned flags") {
    register_engine_components();
    register_engine_components();
    const ComponentInfo &js = ComponentRegistry::global().info<JustSpawned>();
    CHECK(js.flags == (kTag | kSnapshot));
    CHECK(js.size == 0);
    CHECK(js.layout == nullptr);
    CHECK(js.name == "JustSpawned");
    CHECK(ComponentRegistry::global().info<Transform>().on_add != nullptr);
    CHECK(ComponentRegistry::global().info<Parent>().on_remove != nullptr);
    CHECK(ComponentRegistry::global().info<Children>().flags == kSnapshot);
    CHECK(ComponentRegistry::global().info<PrevTransform>().flags == kSnapshot);
}
