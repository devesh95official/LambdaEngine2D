#pragma once
// Spatial queries. Results are sorted by entity index; counts can exceed out.size().
// The grid is rebuilt at the first query of a tick, so if you teleport a body after that, call
// refresh_queries() or later queries that tick may miss it.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <lam/core/id.h>
#include <lam/ecs/entity.h>
#include <lam/math/geometry.h>
#include <lam/physics/components.h>
#include <lam/physics/tile_world.h>
namespace lam {
class World;
struct RayHit {
    Entity entity;                                              // null for tile hits
    Vec2 point;                                                 // circle centre for sweeps
    Vec2 normal;                                                // faces back toward the origin
    float distance = 0;
    bool tile = false;
    IVec2 cell;                                                 // when `tile`
};
// Tiles block as in tile_raycast. The target's own mask isn't checked.
struct RayQuery {
    CollisionMask mask = 0xFFFFFFFFu;
    Entity ignore;                                              // usually the shooter
    bool tiles = true;
    bool triggers = false;
};
// Bodies win ties over tiles, then lower index. Starting inside a collider hits at distance 0.
std::optional<RayHit> raycast(World &w, Vec2 origin, Vec2 dir_unit, float max_dist,
                              CollisionMask mask = 0xFFFFFFFFu, Entity ignore = {});
std::optional<RayHit> raycast(World &w, Vec2 origin, Vec2 dir_unit, float max_dist, const RayQuery &q);
// radius <= 0 behaves like a ray.
std::optional<RayHit> sweep_circle(World &w, Vec2 from, Vec2 to, float radius,
                                   CollisionMask mask = 0xFFFFFFFFu, Entity ignore = {});
std::optional<RayHit> sweep_circle(World &w, Vec2 from, Vec2 to, float radius, const RayQuery &q);
// Exact shapes, triggers included; touching edges don't count.
std::size_t overlap_box(World &w, Rect r, CollisionMask mask, std::span<Entity> out);
std::size_t overlap_circle(World &w, Circle c, CollisionMask mask, std::span<Entity> out);
// Collider centres strictly within `radius`, including the caller.
std::size_t query_neighbors(World &w, Vec2 p, float radius, std::span<Entity> out);
// Each neighbour pushes with strength * (1 - d / radius); the sum is capped at `strength`.
// Bodies on the same spot are split by index so stacks always spread out.
Vec2 separation(World &w, Entity self, float radius, float strength);
// Give a pack its own layer to have it ignore the player.
Vec2 separation(World &w, Entity self, float radius, float strength, CollisionMask mask);
// Same rule over plain positions; no World needed.
Vec2 separation(std::size_t self, std::span<const Vec2> positions, float radius, float strength);
// Nearest Interactable whose reach is within `range` and inside the facing cone (any direction if
// already inside its reach). Ties go to the lower index.
Entity find_interactable(World &w, Vec2 origin, Vec2 facing_unit, float range, float half_angle,
                         StrId kind = {});
// Moves a Box collider the way motors do, for custom controllers. Writes Transform, not Velocity.
BoxMove move_body(World &w, Entity self, Vec2 delta, CollisionMask solid_mask, const MoveBoxOptions &o = {});
void refresh_queries(World &w);
// Inserts an empty TileWorld if missing (bare worlds in tests). An empty one is open space.
TileWorld &ensure_tile_world(World &w);
// Per-tick diagnostics; not hashed or saved.
struct PhysicsStats {                                           // World resource
    std::uint32_t bodies = 0, pairs = 0, dropped_pairs = 0, cells = 0, contacts = 0;
};
}
