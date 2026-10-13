#pragma once
// Transform components and the hierarchy. Transform is the world transform, Parent::local is
// relative to the parent. Children lists stay sorted by entity index so they rebuild identically
// after a restore or load.
#include <cstdint>
#include <lam/core/id.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/transform.h>
namespace lam {
class World;
// World transform: position in pixels, rotation in radians, scale factors.
struct Transform {
    Vec2 position;
    float rotation = 0;
    Vec2 scale{1, 1};
};
// Transform at the start of the current tick (copy_prev_transforms); render lerps from it.
struct PrevTransform {
    Vec2 position;
    float rotation = 0;
    Vec2 scale{1, 1};
};
// Makes this entity a child of `entity`; hooks keep Children and depth up to date.
struct Parent {
    Entity entity;
    Transform local;
    std::uint16_t depth = 0;                                    // engine-maintained
    Entity next_sibling;                                        // engine-maintained
};
// Head of the child list (linked through Parent::next_sibling, ascending entity index).
struct Children {                                               // engine-maintained; never add by hand
    Entity first;
    std::uint16_t count = 0;
};
// A stable name for lookups (find_by_name) and debugging.
struct Name { StrId id; };
struct JustSpawned {};                                          // engine marker
constexpr Transform2D to_transform2d(const Transform &t) { return {t.position, t.rotation, t.scale}; }
constexpr Transform2D to_transform2d(const PrevTransform &t) { return {t.position, t.rotation, t.scale}; }
// Lerps PrevTransform -> Transform. Just-spawned entities get Transform as is; dead ones get {}.
Transform interpolated(const World &w, Entity e, float alpha);
// Immediate. Unlinks any old parent and updates child's Transform.
void set_parent(World &w, Entity child, Entity parent, const Transform &local);
// The child becomes a root and keeps its current world transform.
void clear_parent(World &w, Entity child);
Entity find_by_name(const World &w, StrId name);                // O(n), cold path
// Engine systems (registered by lam_app).
// TickBegin -2000: PrevTransform = Transform for every entity with both. No allocation.
void copy_prev_transforms(World &w);
// Physics -1900 and TickEnd -1000: Transform = parent * local, parents first.
void propagate_parents(World &w);
// TickEnd +1000: PrevTransform = Transform for JustSpawned entities, then drops the marker.
void settle_spawned(World &w);
// TickEnd -900: counts Lifetime down and destroys at 0 through w.commands().
void tick_lifetimes(World &w);
// Registers the engine components and their hooks. Idempotent; every World constructor calls it.
void register_engine_components();
// First restore hook of every World: rebuilds the hierarchy links and any missing PrevTransform.
void restore_engine_state(World &w);
}
LAM_REFLECT(lam::Transform, position, rotation, scale)
LAM_REFLECT_FLAGS(lam::PrevTransform, lam::kSnapshot, position, rotation, scale)
LAM_REFLECT(lam::Parent, entity, local)
LAM_REFLECT_FLAGS(lam::Children, lam::kSnapshot, first, count)
LAM_REFLECT(lam::Name, id)
