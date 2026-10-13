// Transforms, hierarchy hooks and the lam_app engine systems that touch them. Children lists are
// sorted by child index so rebuilds are canonical.
#include <lam/ecs/transform.h>

#include <limits>

#include <lam/core/assert.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/lifetime.h>
#include <lam/ecs/turns.h>
#include <lam/ecs/world.h>

namespace lam {

namespace {

Transform from_2d(const Transform2D &t) {
    return {t.position, t.rotation, t.scale};
}
PrevTransform prev_of(const Transform &t) {
    return {t.position, t.rotation, t.scale};
}

ComponentId id_children() {
    return ComponentRegistry::global().id<Children>();
}
ComponentId id_parent() {
    return ComponentRegistry::global().id<Parent>();
}

// Inserts child into parent's Children list, keeping it sorted by entity index.
void link_child(World &w, Entity parent, Entity child, Parent &pc) {
    Children *ch = w.try_get<Children>(parent);
    if (!ch) ch = &w.add<Children>(parent);
    pc.next_sibling = {};
    if (!ch->first || child.index < ch->first.index) {
        pc.next_sibling = ch->first;
        ch->first = child;
    } else {
        Entity prev = ch->first;
        for (;;) {
            Parent *pp = w.try_get<Parent>(prev);
            if (!pp) break; // corrupt list: append here
            if (!pp->next_sibling || child.index < pp->next_sibling.index) {
                pc.next_sibling = pp->next_sibling;
                pp->next_sibling = child;
                break;
            }
            prev = pp->next_sibling;
        }
    }
    ++ch->count;
}

// Removes child from parent's list; removes Children when it becomes empty.
void unlink_child(World &w, Entity parent, Entity child, Parent &pc) {
    Children *ch = w.try_get<Children>(parent);
    if (!ch) return;
    bool found = false;
    if (ch->first == child) {
        ch->first = pc.next_sibling;
        found = true;
    } else {
        for (Entity prev = ch->first; prev;) {
            Parent *pp = w.try_get<Parent>(prev);
            if (!pp) break;
            if (pp->next_sibling == child) {
                pp->next_sibling = pc.next_sibling;
                found = true;
                break;
            }
            prev = pp->next_sibling;
        }
    }
    pc.next_sibling = {};
    if (!found) return;
    if (ch->count > 0) --ch->count;
    if (ch->count == 0) w.remove<Children>(parent);
}

// Sets depth of every descendant of `e` (whose own depth is `depth`).
void set_subtree_depth(World &w, Entity e, std::uint16_t depth) {
    const Children *ch = w.try_get<Children>(e);
    if (!ch) return;
    const auto next = static_cast<std::uint16_t>(depth < 0xFFFFu ? depth + 1 : depth);
    for (Entity c = ch->first; c;) {
        Parent *pc = w.try_get<Parent>(c);
        if (!pc) break;
        pc->depth = next;
        const Entity sibling = pc->next_sibling;
        set_subtree_depth(w, c, next);
        c = sibling;
    }
}

std::uint16_t depth_of_parent(const World &w, Entity parent) {
    const Parent *pp = w.try_get<Parent>(parent);
    return pp ? pp->depth : 0;
}

bool creates_cycle(const World &w, Entity child, Entity parent) {
    std::size_t guard = w.count<Parent>() + 1;
    for (Entity a = parent; guard-- > 0;) {
        if (a == child) return true;
        const Parent *ap = w.try_get<Parent>(a);
        if (!ap) return false;
        a = ap->entity;
    }
    return true;
}

// Rebuilds every Children list, next_sibling and depth from the Parent components.
void rebuild_hierarchy(World &w) {
    for (;;) {
        const std::span<const Entity> ch = detail::pool_entities(w, id_children());
        if (ch.empty()) break;
        w.remove<Children>(ch.back());
    }
    const std::span<const Entity> kids = detail::pool_entities(w, id_parent());
    for (const Entity c : kids) {
        Parent &pc = w.get<Parent>(c);
        pc.next_sibling = {};
        pc.depth = 0;
    }
    for (const Entity c : kids) {
        Parent &pc = w.get<Parent>(c);
        if (w.alive(pc.entity) && pc.entity != c) link_child(w, pc.entity, c, pc);
    }
    const std::size_t limit = kids.size() + 1;
    for (const Entity c : kids) {
        std::size_t depth = 0;
        Entity a = c;
        while (depth < limit) {
            const Parent *ap = w.try_get<Parent>(a);
            if (!ap || !w.alive(ap->entity)) break;
            ++depth;
            a = ap->entity;
        }
        w.get<Parent>(c).depth = static_cast<std::uint16_t>(depth < 0xFFFFu ? depth : 0xFFFFu);
    }
}

void on_transform_add(World &w, Entity e, void *p) {
    const PrevTransform prev = prev_of(*static_cast<const Transform *>(p));
    w.add<PrevTransform>(e, prev);
}

void on_parent_add(World &w, Entity child, void *p) {
    Parent &pc = *static_cast<Parent *>(p);
    pc.next_sibling = {};
    pc.depth = 0;
    const Entity parent = pc.entity;
    const bool ok = w.alive(parent) && parent != child && !creates_cycle(w, child, parent);
    LAM_ASSERT(ok,
               "Parent: the parent must be alive, different from the child and not its descendant");
    if (!ok) return;
    link_child(w, parent, child, pc);
    Parent &linked = w.get<Parent>(child); // Children insert may not move Parent; re-fetch anyway
    linked.depth = static_cast<std::uint16_t>(depth_of_parent(w, parent) + 1u);
    set_subtree_depth(w, child, linked.depth);
}

void on_parent_replace(World &w, Entity, void *) {
    // The old link fields were overwritten by the assignment: relink everything (cold path).
    rebuild_hierarchy(w);
}

void on_parent_remove(World &w, Entity child, void *p) {
    Parent &pc = *static_cast<Parent *>(p);
    if (w.alive(pc.entity)) unlink_child(w, pc.entity, child, pc);
    pc.depth = 0;
    set_subtree_depth(w, child, 0);
}

void mark_subtree_spawned(World &w, Entity e) {
    w.add<JustSpawned>(e);
    const Children *ch = w.try_get<Children>(e);
    if (!ch) return;
    for (Entity c = ch->first; c;) {
        const Parent *pc = w.try_get<Parent>(c);
        if (!pc) break;
        const Entity sibling = pc->next_sibling;
        mark_subtree_spawned(w, c);
        c = sibling;
    }
}

} // namespace

void register_engine_components() {
    static const bool once = [] {
        ComponentRegistry &r = ComponentRegistry::global();
        r.set_hooks<Transform>(&on_transform_add, nullptr, nullptr);
        (void)r.id<PrevTransform>();
        r.set_hooks<Parent>(&on_parent_add, &on_parent_replace, &on_parent_remove);
        (void)r.id<Children>();
        (void)r.id<Name>();
        r.register_type<JustSpawned>(kTag | kSnapshot);
        (void)r.id<Lifetime>();
        (void)r.id<TurnActor>();
        (void)r.id<PlayerTurn>();
        return true;
    }();
    (void)once;
}

void restore_engine_state(World &w) {
    rebuild_hierarchy(w);
    const std::span<const Entity> ents =
        detail::pool_entities(w, ComponentRegistry::global().id<Transform>());
    for (std::size_t i = 0; i < ents.size(); ++i) {
        const Entity e = ents[i];
        if (w.has<PrevTransform>(e)) continue;
        const PrevTransform prev = prev_of(w.get<Transform>(e));
        w.add<PrevTransform>(e, prev);
    }
}

Transform interpolated(const World &w, Entity e, float alpha) {
    const Transform *t = w.try_get<Transform>(e);
    if (!t) return {};
    const PrevTransform *p = w.try_get<PrevTransform>(e);
    if (!p || w.has<JustSpawned>(e)) return *t;
    return from_2d(lerp(to_transform2d(*p), to_transform2d(*t), alpha));
}

void set_parent(World &w, Entity child, Entity parent, const Transform &local) {
    LAM_ASSERT(w.alive(child) && w.alive(parent), "set_parent: both entities must be alive");
    if (!w.alive(child) || !w.alive(parent)) return;
    if (w.has<Parent>(child)) w.remove<Parent>(child);
    Parent pc;
    pc.entity = parent;
    pc.local = local;
    w.add<Parent>(child, pc);
    const Transform *pt = w.try_get<Transform>(parent);
    const Transform world =
        pt ? from_2d(combine(to_transform2d(*pt), to_transform2d(local))) : local;
    if (Transform *ct = w.try_get<Transform>(child))
        *ct = world;
    else
        w.add<Transform>(child, world);
}

void clear_parent(World &w, Entity child) {
    w.remove<Parent>(child);
}

Entity find_by_name(const World &w, StrId name) {
    for (const Entity e : detail::pool_entities(w, ComponentRegistry::global().id<Name>()))
        if (w.get<Name>(e).id == name) return e;
    return {};
}

void copy_prev_transforms(World &w) {
    w.view<Transform, PrevTransform>().each(
        [](Entity, Transform &t, PrevTransform &p) { p = prev_of(t); });
}

void propagate_parents(World &w) {
    std::span<const Entity> kids = detail::pool_entities(w, id_parent());
    if (kids.empty()) return;
    bool sorted = true;
    for (std::size_t i = 1; i < kids.size() && sorted; ++i)
        sorted = w.get<Parent>(kids[i - 1]).depth <= w.get<Parent>(kids[i]).depth;
    if (!sorted) {
        w.sort<Parent>([](const Parent &a, const Parent &b) { return a.depth < b.depth; });
        kids = detail::pool_entities(w, id_parent());
    }
    for (const Entity c : kids) {
        const Parent &pc = w.get<Parent>(c);
        const Transform *pt = w.try_get<Transform>(pc.entity);
        Transform *ct = w.try_get<Transform>(c);
        if (pt && ct) *ct = from_2d(combine(to_transform2d(*pt), to_transform2d(pc.local)));
    }
}

void settle_spawned(World &w) {
    const ComponentId js = ComponentRegistry::global().id<JustSpawned>();
    for (;;) {
        const std::span<const Entity> ents = detail::pool_entities(w, js);
        if (ents.empty()) break;
        const Entity e = ents.back();
        if (const Transform *t = w.try_get<Transform>(e)) {
            if (PrevTransform *p = w.try_get<PrevTransform>(e)) *p = prev_of(*t);
        }
        w.remove<JustSpawned>(e);
    }
}

void tick_lifetimes(World &w) {
    Commands &c = w.commands();
    w.view<Lifetime>().each([&](Entity e, Lifetime &l) {
        if (l.ticks > 0) --l.ticks;
        if (l.ticks == 0) c.destroy(e);
    });
}

void World::teleport(Entity e, Vec2 position) {
    LAM_ASSERT(alive(e), "World::teleport: entity is not alive");
    if (!alive(e)) return;
    Transform t;
    if (const Transform *cur = try_get<Transform>(e)) t = *cur;
    t.position = position;
    if (Parent *pc = try_get<Parent>(e)) {
        if (const Transform *pt = try_get<Transform>(pc->entity)) {
            pc->local.position = to_transform2d(*pt).matrix().inverse().apply(position);
        } else {
            pc->local.position = position;
        }
    }
    add<Transform>(e, t); // new: on_add adds PrevTransform
    add<PrevTransform>(e, prev_of(t));
    mark_subtree_spawned(*this, e);
}

} // namespace lam
