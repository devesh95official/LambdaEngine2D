#pragma once
// Physics events, read in PostPhysics. Contacts are diffed against last tick's set, which is
// saved with the world, so a load or undo fires the same events as an uninterrupted run.
#include <lam/ecs/entity.h>
#include <lam/math/vec2.h>
namespace lam {
// Fired once per touch (overlap or within 1/32 px). a.index < b.index, normal points from a to b.
// Use other() and normal_from() rather than checking which side you are.
struct CollisionStarted { Entity a, b; Vec2 normal; };
// Either entity may already be dead.
struct CollisionEnded { Entity a, b; };
// If both are triggers, each reports its own event.
struct TriggerEntered { Entity trigger, other; };
struct TriggerExited { Entity trigger, other; };
// Repeats every tick while a moving body touches a hazard tile, so debounce it in game code.
struct TileHazardTouched { Entity entity; IVec2 tile; };
// target is null when tile is true. point is the projectile's centre at contact; normal faces back
// toward where it came from. Things already overlapped at the start of a tick aren't hit again.
struct ProjectileHit { Entity projectile, target; Vec2 point, normal; bool tile = false; };
// Game code emits this after find_interactable.
struct Interacted { Entity actor, target; };

// Null if `self` is neither a nor b.
constexpr Entity other(const CollisionStarted &c, Entity self) {
    return c.a == self ? c.b : (c.b == self ? c.a : Entity{});
}
constexpr Entity other(const CollisionEnded &c, Entity self) {
    return c.a == self ? c.b : (c.b == self ? c.a : Entity{});
}
// Normal pointing from `self` to the other entity.
constexpr Vec2 normal_from(const CollisionStarted &c, Entity self) {
    return c.a == self ? c.normal : (c.b == self ? Vec2{-c.normal.x, -c.normal.y} : Vec2{});
}
}
