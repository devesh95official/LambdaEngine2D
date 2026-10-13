#pragma once
// Private per-world physics state: body caches, the static and dynamic grids, pairs and solver
// contacts. It's a cache and isn't snapshotted; PhysicsContactSet below is the saved part.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <lam/core/data.h>
#include <lam/core/hash.h>
#include <lam/core/result.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/rect.h>
#include <lam/math/vec2.h>
#include <lam/physics/components.h>
#include <lam/physics/events.h>
#include <lam/physics/physics_plugin.h>
#include <lam/physics/queries.h>
#include <lam/physics/tile_world.h>

#include "detail/shapes.h"

namespace lam {
class World;
struct Transform;
}

namespace lam::detail {

// Shapes this close count as touching, so bodies resting flush stay in contact.
inline constexpr float kContactSkin = 1.0f / 32.0f;
// Body references: index into PhysicsWorld::bodies, or (kStaticRef | index) into statics.
inline constexpr std::uint32_t kStaticRef = 0x80000000u;
inline constexpr std::uint32_t kNoRef = 0xFFFFFFFFu;
// A body covering more cells than this is kept out of the grid and tested against everything.
inline constexpr std::int64_t kMaxCellsPerBody = 256;

enum BodyFlag : std::uint16_t {
    kBodyTrigger = 1,
    kBodyOneWay = 2,
    kBodyMotor = 4,                                             // PlatformerMotor or TopDownMotor
    kBodySleeping = 8,                                          // RigidBody::sleeping
    kBodyVelocity = 16,                                         // has Velocity
    kBodyProjectile = 32,
};

// One collider in world space, as seen by the broadphase and the narrowphase.
struct Body {
    Entity entity;
    ShapeView shape;                                            // shape.aabb: bounds without skin
    Rect grown;                                                 // shape.aabb grown by kContactSkin
    CollisionMask layer = 1;
    CollisionMask mask = 0xFFFFFFFFu;
    BodyType type = BodyType::Static;
    std::uint16_t flags = 0;
};

// True when a and b may interact: each one's layer is in the other's mask.
inline bool masks_match(const Body &a, const Body &b) {
    return (a.layer & b.mask) != 0 && (b.layer & a.mask) != 0;
}

// One (cell, body) entry of a spatial hash.
struct CellEntry {
    std::int32_t cx = 0;
    std::int32_t cy = 0;
    std::uint32_t ref = 0;
};

// Inclusive cell range of a box.
struct CellRange {
    std::int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;
};

// A uniform spatial hash built by counting sort: entries are grouped by hash bucket, and within a
// bucket kept in body order, so the layout is a pure function of the body list.
class CellGrid {
public:
    // refs = ref_base | index. Non-finite bodies are skipped; huge ones go to oversize().
    void build(std::span<const Body> bodies, std::uint32_t ref_base, float cell_size);
    void clear();
    float cell_size() const { return cell_; }
    // The cells a box (grown by kContactSkin) covers, clamped to +-2^30.
    CellRange range_of(const Rect &box) const;
    std::int32_t cell_of(float v) const;
    // Entries of the hash bucket holding cell (cx, cy); filter on cx/cy (buckets are shared).
    std::span<const CellEntry> bucket_for(std::int32_t cx, std::int32_t cy) const;
    std::span<const CellEntry> bucket(std::uint32_t b) const;
    std::uint32_t bucket_count() const { return mask_ + 1u; }
    std::span<const std::uint32_t> oversize() const { return oversize_; }
    std::size_t entry_count() const { return entries_.size(); }
    std::uint32_t occupied_buckets() const { return occupied_; }
    bool empty() const { return entries_.empty() && oversize_.empty(); }

private:
    std::uint32_t hash(std::int32_t cx, std::int32_t cy) const {
        const std::uint32_t h = static_cast<std::uint32_t>(cx) * 73856093u ^ static_cast<std::uint32_t>(cy) * 19349663u;
        return (h ^ (h >> 15)) & mask_;
    }
    float cell_ = 64.0f;
    float inv_cell_ = 1.0f / 64.0f;
    std::uint32_t mask_ = 0;
    std::uint32_t occupied_ = 0;
    std::vector<std::uint32_t> starts_;                         // bucket_count + 1
    std::vector<CellEntry> entries_;
    std::vector<CellRange> ranges_;                             // per body (scratch)
    std::vector<std::uint32_t> oversize_;
};

// A candidate pair: refs ordered so that the first has the lower entity index.
struct Pair {
    std::uint64_t key = 0;                                      // (index a << 32) | index b
    std::uint32_t a = 0;
    std::uint32_t b = 0;
};

// One overlap found by the narrowphase, the unit of the previous-contact set.
struct ContactRecord {
    Entity first;                                               // collision: lower index; trigger: the trigger
    Entity second;
    Vec2 normal;                                                // collision: first -> second
    std::uint8_t kind = 0;                                      // 0 collision, 1 trigger
};

// A velocity/position constraint between a dynamic body and another body.
struct SolverContact {
    std::uint32_t a = 0, b = 0;                                 // body refs
    std::uint32_t da = kNoRef, db = kNoRef;                     // dynamic slots (kNoRef: immovable)
    Vec2 normal;                                                // a -> b
    float depth = 0;
    float restitution = 0;
    float friction = 0;
    float normal_impulse = 0;
    float tangent_impulse = 0;
    float bounce = 0;                                           // target separating speed
    float ima = 0, imb = 0;                                     // effective inverse masses (solve)
    Vec2 kva, kvb;                                              // velocities of immovable sides (solve)
};

// Per dynamic body solver state.
struct DynState {
    std::uint32_t ref = 0;
    Vec2 velocity;
    Vec2 correction;                                            // accumulated position push
    float inv_mass = 0;
    std::uint8_t tile_block = 0;                                // bits: 1 -x, 2 +x, 4 -y, 8 +y touching tiles
};

struct PhysicsWorld {
    explicit PhysicsWorld(const PhysicsConfig &c);
    PhysicsConfig config;

    // Bodies
    std::vector<Body> bodies;                                   // non-static, current build
    std::vector<Body> statics;
    std::vector<Vec2> static_positions;                         // Transform.position per static (verify)
    std::vector<Collider> static_colliders;                     // Collider per static (verify)
    std::array<std::uint32_t, 7> class_versions{};              // pools deciding body types, at the static build
    CellGrid dyn;
    CellGrid stat;
    bool dyn_valid = false;                                     // dyn reflects a gather of this world
    float margin = 0;                                           // max displacement since the dyn build
    std::uint64_t dyn_tick = ~0ull;                             // world tick of the last dyn build
    bool dyn_fresh = false;                                     // built by projectiles; broadphase may reuse
    std::array<std::uint32_t, 10> versions{};                   // structural stamp of the dyn build
    std::size_t static_polygons = 0;                            // PolygonShapes size at the static build
    std::uint32_t static_collider_version = 0;                  // Collider pool version seen by update_static
    std::size_t static_total = 0;                               // statics met by the last static rebuild
    bool static_dirty = true;
    std::size_t statics_seen = 0;                               // statics met by the last gather
    std::uint64_t static_builds = 0;                            // diagnostics (tests)
    std::uint64_t dyn_builds = 0;
    // Layers of every solid (non-trigger Box) body. Lets a motor skip the solid query when its
    // solid_mask can't match anything, so crowds that block nothing stay cheap.
    CollisionMask dyn_solid_layers = 0;
    CollisionMask static_solid_layers = 0;
    bool solid_layers_fresh = false;

    // Pairs and contacts
    std::vector<Pair> pairs;
    std::vector<Pair> pair_scratch;
    std::uint32_t dropped_pairs = 0;
    std::vector<ContactRecord> contacts;                        // this tick, sorted (last tick's: PhysicsContactSet)
    std::vector<SolverContact> solver;
    std::vector<DynState> dyn_state;
    std::vector<std::uint32_t> dyn_slot;                        // body index -> dyn_state slot

    // Query scratch
    std::vector<std::uint32_t> visit;                           // per ref (dyn, then statics)
    std::uint32_t visit_id = 0;
    std::vector<Entity> found;
    std::vector<Rect> solid_rects;
    std::vector<Rect> one_way_rects;
    std::vector<Entity> solid_entities;                         // solids, then one-ways
    struct Hit {
        float t = 0;
        Entity entity;
        Vec2 point, normal;
    };
    std::vector<Hit> hits;
    std::vector<ProjectileHit> projectile_hits;                 // this tick, emitted by the system
};

// Last tick's contacts, saved so an undo or load fires the same events as an uninterrupted run.
// Copy-assignment sets `restored` so the restore hook can spot old saves without it.
struct PhysicsContactSet {
    std::vector<ContactRecord> records;
    bool restored = false;
    PhysicsContactSet() = default;
    PhysicsContactSet(const PhysicsContactSet &o) : records(o.records), restored(true) {}
    PhysicsContactSet &operator=(const PhysicsContactSet &o) {
        records = o.records;
        restored = true;
        return *this;
    }
    PhysicsContactSet(PhysicsContactSet &&) noexcept = default;
    PhysicsContactSet &operator=(PhysicsContactSet &&) noexcept = default;
};
// {"records": [[first.index, first.generation, second.index, second.generation, kind], ...]}.
Status lam_read(PhysicsContactSet &s, DataReader &in, const SerializeContext &ctx);
void lam_write(const PhysicsContactSet &s, DataWriter &out, const SerializeContext &ctx);
void lam_hash(const PhysicsContactSet &s, Hasher &h);

// ---- Body gathering and grids (broadphase.cpp) -------------------------------------------------

// Which type-deciding pools are non-empty, so classify() can skip lookups into empty ones.
struct BodyPools {
    bool rigid = true, velocity = true, platformer = true, top_down = true, moving = true, path = true,
         projectile = true;
};
BodyPools body_pools(const World &w);
// Static, Kinematic or Dynamic per the components.h rule.
BodyType classify(const World &w, Entity e, const RigidBody *rb);
BodyType classify(const World &w, Entity e, const RigidBody *rb, const BodyPools &pools);
// Fills `out` for the collider of e at `position`; false if the bounds are not finite.
bool make_body(const World &w, Entity e, const Collider &c, Vec2 position, const PolygonShapes *polygons,
               Body &out);
bool make_body(const World &w, Entity e, const Collider &c, Vec2 position, const PolygonShapes *polygons,
               const BodyPools &pools, Body &out);
// Gathers every non-static body and rebuilds the dynamic grid (margin = 0). Counts the statics
// it meets and rebuilds the static grid if that count differs from the cache.
void build_dynamic(World &w, PhysicsWorld &pw);
// Rebuilds the static grid if anything static changed. `verify` also checks every cached static.
bool update_static(World &w, PhysicsWorld &pw, bool verify);
// Makes sure both grids exist and reflect the current set of bodies (queries and hooks).
void ensure_grids(World &w, PhysicsWorld &pw);
// Body by reference.
inline const Body &body_at(const PhysicsWorld &pw, std::uint32_t ref) {
    return (ref & kStaticRef) != 0 ? pw.statics[ref & ~kStaticRef] : pw.bodies[ref];
}
// Calls f(ref) once per body whose cells touch `region`. Candidates only; test the real shapes.
template <class F> void for_candidates(PhysicsWorld &pw, const Rect &region, F &&f);
// Sorted by (a.index, b.index); anything past pair_capacity is dropped and counted.
void find_pairs(PhysicsWorld &pw);

// ---- Contacts and solver (solver.cpp) ----------------------------------------------------------

// Narrowphase over pw.pairs into pw.contacts (sorted) and, if `solve`, the solver contacts.
void narrowphase(World &w, PhysicsWorld &pw, bool solve);
// Sequential impulses then position correction; writes Velocity and Transform of dynamic bodies.
void solve_contacts(World &w, PhysicsWorld &pw, float dt);
// Integrates Velocity (and RigidBody gravity/damping) for non-motor bodies, sweeping dynamic
// bodies against the tile grid.
void integrate_bodies(World &w, PhysicsWorld &pw, Vec2 gravity, float dt);

// ---- Tile helpers (tile_world.cpp) -------------------------------------------------------------

struct RigidMove {
    Rect box;
    bool hit_x = false, hit_y = false;
    bool started_blocked = false;                               // box overlapped a Solid tile: not moved
};
// A rigid body's box against the grid: Solid tiles block, OneWay tiles block falling onto them,
// slopes do not block (they are resolved by shape afterwards). Exact edges, no tunnelling.
RigidMove move_rigid(const TileWorld &w, Rect box, Vec2 delta);
// True when a Solid tile overlaps the box.
bool rigid_blocked(const TileWorld &w, const Rect &box);
// First tile (row-major) overlapping `box` whose flags include `flag`.
bool find_flag(const TileWorld &w, const Rect &box, std::uint8_t flag, IVec2 &cell);

// ---- Motors (motors.cpp) -----------------------------------------------------------------------

// Against a wall the blocked axis restarts from rest and the free axis gets all the acceleration.
Vec2 step_velocity_walled(Vec2 v, Vec2 input, float max_speed, float accel, float decel, float dt, IVec2 wall);
// |input| <= 1 and finite (NaN -> 0).
Vec2 clamp_input(Vec2 input);
// Fills pw.solid_rects/one_way_rects (+ entities) with the entity solids near `region` for a body
// `self` with `solid_mask`. one_way colliders go to one_way_rects when `platformer`.
void gather_solids(World &w, PhysicsWorld &pw, Entity self, const Rect &region, CollisionMask solid_mask,
                   bool platformer);
// Physics -1800: PathFollowers move their Transform (and report it in Velocity).
void run_path_followers(World &w, PhysicsWorld &pw, float dt);
// Physics -1800, after path followers: Velocity-driven platforms move; last_delta is set.
void run_moving_platforms(World &w, PhysicsWorld &pw, float dt);
void run_carry_riders(World &w, PhysicsWorld &pw);
void run_motors(World &w, PhysicsWorld &pw, float dt);

// ---- Projectiles (projectiles.cpp) -------------------------------------------------------------

// Appends hits to pw.projectile_hits in sweep order.
void run_projectiles(World &w, PhysicsWorld &pw, float dt);

// ---- Grid traversal (template definitions) -----------------------------------------------------

template <class F> void for_candidates(PhysicsWorld &pw, const Rect &region, F &&f) {
    if (!(region.w >= 0.0f) || !(region.h >= 0.0f)) return;
    const Rect grown = region.expanded(pw.margin);
    const std::size_t n_dyn = pw.bodies.size();
    if (pw.visit.size() < n_dyn + pw.statics.size()) pw.visit.resize(n_dyn + pw.statics.size(), 0);
    if (++pw.visit_id == 0) {
        for (std::uint32_t &v : pw.visit) v = 0;
        pw.visit_id = 1;
    }
    const std::uint32_t id = pw.visit_id;
    const auto visit = [&](std::uint32_t ref) {
        const std::size_t slot = (ref & kStaticRef) != 0 ? n_dyn + (ref & ~kStaticRef) : ref;
        if (slot >= pw.visit.size() || pw.visit[slot] == id) return;
        pw.visit[slot] = id;
        f(ref);
    };
    for (const CellGrid *g : {&pw.stat, &pw.dyn}) {
        for (const std::uint32_t ref : g->oversize()) visit(ref);
        if (g->entry_count() == 0) continue;
        const CellRange r = g->range_of(grown);
        const std::int64_t cells = (static_cast<std::int64_t>(r.x1) - r.x0 + 1) * (static_cast<std::int64_t>(r.y1) - r.y0 + 1);
        if (cells > static_cast<std::int64_t>(g->bucket_count())) {
            // Bigger than the table: walk every bucket once instead.
            for (std::uint32_t b = 0; b < g->bucket_count(); ++b) {
                for (const CellEntry &e : g->bucket(b)) {
                    if (e.cx >= r.x0 && e.cx <= r.x1 && e.cy >= r.y0 && e.cy <= r.y1) visit(e.ref);
                }
            }
            continue;
        }
        for (std::int32_t cy = r.y0; cy <= r.y1; ++cy) {
            for (std::int32_t cx = r.x0; cx <= r.x1; ++cx) {
                for (const CellEntry &e : g->bucket_for(cx, cy)) {
                    if (e.cx == cx && e.cy == cy) visit(e.ref);
                }
            }
        }
    }
}

}

LAM_REFLECT_CUSTOM(lam::detail::PhysicsContactSet, lam::kSnapshot | lam::kSerialize)
