// Broadphase. Statics get their own grid, rebuilt only when they change; everything else is
// counting-sorted into a grid each tick. A pair is reported only from the cell holding the min
// corner of the overlap, so it's found once. Overflow drops the largest pair keys.
#include <algorithm>
#include <cmath>

#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/motors.h>

#include "detail/physics_world.h"

namespace lam::detail {
namespace {

constexpr float kCellLimit = 1073741824.0f;                    // 2^30

bool finite_rect(const Rect &r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h);
}

std::uint32_t next_pow2(std::size_t n) {
    std::uint32_t p = 64;
    while (p < n && p < 0x40000000u) p <<= 1;
    return p;
}

// The structural stamp of the things a gather depends on.
std::array<std::uint32_t, 10> dyn_stamp(const World &w) {
    const auto *polys = w.resources().try_get<PolygonShapes>();
    return {w.pool_version<Transform>(),     w.pool_version<Collider>(),     w.pool_version<RigidBody>(),
            w.pool_version<Velocity>(),      w.pool_version<PlatformerMotor>(), w.pool_version<TopDownMotor>(),
            w.pool_version<MovingPlatform>(), w.pool_version<PathFollower>(), w.pool_version<Projectile>(),
            static_cast<std::uint32_t>(polys ? polys->verts.size() : 0)};
}

// The pools whose structural changes can change a body's type.
std::array<std::uint32_t, 7> class_stamp(const World &w) {
    return {w.pool_version<RigidBody>(),      w.pool_version<Velocity>(),     w.pool_version<PlatformerMotor>(),
            w.pool_version<TopDownMotor>(),   w.pool_version<MovingPlatform>(), w.pool_version<PathFollower>(),
            w.pool_version<Projectile>()};
}

bool same_collider(const Collider &a, const Collider &b) {
    return a.shape == b.shape && a.offset == b.offset && a.half == b.half && a.radius == b.radius &&
           a.polygon == b.polygon && a.layer == b.layer && a.mask == b.mask && a.trigger == b.trigger &&
           a.one_way == b.one_way;
}

// LSD radix sort, skipping digits that are the same for every pair.
void sort_pairs(std::vector<Pair> &pairs, std::vector<Pair> &scratch) {
    const std::size_t n = pairs.size();
    if (n < 2) return;
    if (n < 64) {
        std::sort(pairs.begin(), pairs.end(), [](const Pair &a, const Pair &b) { return a.key < b.key; });
        return;
    }
    scratch.resize(n);
    std::array<std::array<std::uint32_t, 256>, 8> counts{};
    for (const Pair &p : pairs) {
        for (int d = 0; d < 8; ++d) ++counts[static_cast<std::size_t>(d)][(p.key >> (d * 8)) & 0xFFu];
    }
    Pair *src = pairs.data();
    Pair *dst = scratch.data();
    for (int d = 0; d < 8; ++d) {
        auto &c = counts[static_cast<std::size_t>(d)];
        bool trivial = false;
        for (const std::uint32_t v : c) {
            if (v == n) trivial = true;
        }
        if (trivial) continue;
        std::uint32_t sum = 0;
        for (std::uint32_t &v : c) {
            const std::uint32_t k = v;
            v = sum;
            sum += k;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t digit = static_cast<std::uint32_t>((src[i].key >> (d * 8)) & 0xFFu);
            dst[c[digit]++] = src[i];
        }
        std::swap(src, dst);
    }
    if (src != pairs.data()) std::copy(src, src + n, pairs.data());
}

struct PairSink {
    PhysicsWorld *pw;
    std::size_t capacity;
    bool heap = false;

    static bool key_less(const Pair &a, const Pair &b) { return a.key < b.key; }

    void add(std::uint32_t ra, std::uint32_t rb) {
        const Body &a = body_at(*pw, ra);
        const Body &b = body_at(*pw, rb);
        if (!masks_match(a, b)) return;
        // Keep pairs the solver ignores; they still report contacts.
        Pair p;
        if (a.entity.index < b.entity.index) {
            p = Pair{(static_cast<std::uint64_t>(a.entity.index) << 32) | b.entity.index, ra, rb};
        } else {
            p = Pair{(static_cast<std::uint64_t>(b.entity.index) << 32) | a.entity.index, rb, ra};
        }
        std::vector<Pair> &v = pw->pairs;
        if (v.size() < capacity) {
            v.push_back(p);
            return;
        }
        ++pw->dropped_pairs;
        if (capacity == 0) return;
        if (!heap) {
            std::make_heap(v.begin(), v.end(), key_less);
            heap = true;
        }
        if (p.key < v.front().key) {
            std::pop_heap(v.begin(), v.end(), key_less);
            v.back() = p;
            std::push_heap(v.begin(), v.end(), key_less);
        }
    }
};

bool grown_overlap(const Body &a, const Body &b) { return a.grown.overlaps(b.grown); }

bool owns_pair(const CellGrid &g, const Body &a, const Body &b, std::int32_t cx, std::int32_t cy) {
    const Rect &ea = a.grown;
    const Rect &eb = b.grown;
    return g.cell_of(ea.x > eb.x ? ea.x : eb.x) == cx && g.cell_of(ea.y > eb.y ? ea.y : eb.y) == cy;
}

bool is_oversize(std::span<const std::uint32_t> list, std::uint32_t ref) {
    return std::binary_search(list.begin(), list.end(), ref);
}

} // namespace

// ---- CellGrid ------------------------------------------------------------------------------------

std::int32_t CellGrid::cell_of(float v) const {
    float c = std::floor(v * inv_cell_);
    if (!(c > -kCellLimit)) c = -kCellLimit;
    if (c > kCellLimit) c = kCellLimit;
    return static_cast<std::int32_t>(c);
}

CellRange CellGrid::range_of(const Rect &box) const {
    const Rect g = box.expanded(kContactSkin);
    return CellRange{cell_of(g.x), cell_of(g.y), cell_of(g.x + g.w), cell_of(g.y + g.h)};
}

void CellGrid::clear() {
    entries_.clear();
    oversize_.clear();
    occupied_ = 0;
}

std::span<const CellEntry> CellGrid::bucket(std::uint32_t b) const {
    if (entries_.empty() || b > mask_) return {};
    return std::span<const CellEntry>(entries_.data() + starts_[b], starts_[b + 1] - starts_[b]);
}

std::span<const CellEntry> CellGrid::bucket_for(std::int32_t cx, std::int32_t cy) const {
    if (entries_.empty()) return {};
    return bucket(hash(cx, cy));
}

void CellGrid::build(std::span<const Body> bodies, std::uint32_t ref_base, float cell_size) {
    cell_ = cell_size > 0.0f && std::isfinite(cell_size) ? cell_size : 64.0f;
    inv_cell_ = 1.0f / cell_;
    entries_.clear();
    oversize_.clear();
    occupied_ = 0;
    ranges_.resize(bodies.size());
    std::size_t total = 0;
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        const Rect &box = bodies[i].shape.aabb;
        ranges_[i] = CellRange{};
        if (!finite_rect(box)) continue;
        const CellRange r = range_of(box);
        const std::int64_t cells = (static_cast<std::int64_t>(r.x1) - r.x0 + 1) * (static_cast<std::int64_t>(r.y1) - r.y0 + 1);
        if (cells > kMaxCellsPerBody) {
            oversize_.push_back(ref_base | static_cast<std::uint32_t>(i));
            continue;
        }
        ranges_[i] = r;
        total += static_cast<std::size_t>(cells);
    }
    const std::uint32_t table = next_pow2(total * 2);
    mask_ = table - 1u;
    starts_.assign(static_cast<std::size_t>(table) + 1u, 0u);
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        const CellRange &r = ranges_[i];
        for (std::int32_t cy = r.y0; cy <= r.y1; ++cy) {
            for (std::int32_t cx = r.x0; cx <= r.x1; ++cx) ++starts_[hash(cx, cy) + 1u];
        }
    }
    for (std::uint32_t b = 0; b < table; ++b) {
        if (starts_[b + 1u] != 0) ++occupied_;
        starts_[b + 1u] += starts_[b];
    }
    entries_.resize(total);
    // Use starts_ as running cursors, then shift them back below.
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        const CellRange &r = ranges_[i];
        for (std::int32_t cy = r.y0; cy <= r.y1; ++cy) {
            for (std::int32_t cx = r.x0; cx <= r.x1; ++cx) {
                const std::uint32_t b = hash(cx, cy);
                entries_[starts_[b]++] = CellEntry{cx, cy, ref_base | static_cast<std::uint32_t>(i)};
            }
        }
    }
    for (std::uint32_t b = table; b > 0; --b) starts_[b] = starts_[b - 1u];
    starts_[0] = 0;
}

// ---- Bodies ----------------------------------------------------------------------------------------

BodyPools body_pools(const World &w) {
    BodyPools p;
    p.rigid = w.count<RigidBody>() != 0;
    p.velocity = w.count<Velocity>() != 0;
    p.platformer = w.count<PlatformerMotor>() != 0;
    p.top_down = w.count<TopDownMotor>() != 0;
    p.moving = w.count<MovingPlatform>() != 0;
    p.path = w.count<PathFollower>() != 0;
    p.projectile = w.count<Projectile>() != 0;
    return p;
}

BodyType classify(const World &w, Entity e, const RigidBody *rb, const BodyPools &p) {
    if (rb != nullptr) return rb->type;
    if ((p.velocity && w.has<Velocity>(e)) || (p.platformer && w.has<PlatformerMotor>(e)) ||
        (p.top_down && w.has<TopDownMotor>(e)) || (p.moving && w.has<MovingPlatform>(e)) ||
        (p.path && w.has<PathFollower>(e)) || (p.projectile && w.has<Projectile>(e))) {
        return BodyType::Kinematic;
    }
    return BodyType::Static;
}

BodyType classify(const World &w, Entity e, const RigidBody *rb) { return classify(w, e, rb, BodyPools{}); }

bool make_body(const World &w, Entity e, const Collider &c, Vec2 position, const PolygonShapes *polygons,
               const BodyPools &p, Body &out) {
    out.entity = e;
    out.shape = make_shape(c, position, polygons);
    if (!finite_rect(out.shape.aabb) || !is_finite(out.shape.center)) return false;
    out.grown = out.shape.aabb.expanded(kContactSkin);
    out.layer = c.layer;
    out.mask = c.mask;
    std::uint16_t f = 0;
    if (c.trigger) f |= kBodyTrigger;
    if (c.one_way) f |= kBodyOneWay;
    if (p.velocity && w.has<Velocity>(e)) f |= kBodyVelocity;
    if ((p.platformer && w.has<PlatformerMotor>(e)) || (p.top_down && w.has<TopDownMotor>(e))) f |= kBodyMotor;
    if (p.projectile && w.has<Projectile>(e)) f |= kBodyProjectile;
    if (p.rigid) {
        if (const RigidBody *rb = w.try_get<RigidBody>(e); rb != nullptr && rb->sleeping) f |= kBodySleeping;
    }
    out.flags = f;
    return true;
}

bool make_body(const World &w, Entity e, const Collider &c, Vec2 position, const PolygonShapes *polygons,
               Body &out) {
    return make_body(w, e, c, position, polygons, BodyPools{}, out);
}

PhysicsWorld::PhysicsWorld(const PhysicsConfig &c) : config(c) {
    if (!(config.cell_size > 0.0f) || !std::isfinite(config.cell_size)) config.cell_size = 64.0f;
    if (config.solver_iterations < 1) config.solver_iterations = 1;
    bodies.reserve(config.body_capacity);
    statics.reserve(config.body_capacity / 4);
    static_positions.reserve(config.body_capacity / 4);
    static_colliders.reserve(config.body_capacity / 4);
    pairs.reserve(config.pair_capacity);
    pair_scratch.reserve(config.pair_capacity);
    contacts.reserve(config.pair_capacity / 8);
    solver.reserve(config.pair_capacity / 8);
    dyn_state.reserve(config.body_capacity);
    dyn_slot.reserve(config.body_capacity);
    visit.reserve(config.body_capacity + config.body_capacity / 4);
    found.reserve(config.body_capacity);
    solid_rects.reserve(256);
    one_way_rects.reserve(64);
    solid_entities.reserve(320);
    hits.reserve(256);
    projectile_hits.reserve(256);
}

void build_dynamic(World &w, PhysicsWorld &pw) {
    pw.bodies.clear();
    pw.dyn_solid_layers = 0;
    std::size_t statics = 0;
    const PolygonShapes *polygons = w.resources().try_get<PolygonShapes>();
    const BodyPools pools = body_pools(w);
    for (auto [e, t, c] : w.view<Transform, Collider>()) {
        const RigidBody *rb = pools.rigid ? w.try_get<RigidBody>(e) : nullptr;
        const BodyType type = classify(w, e, rb, pools);
        if (type == BodyType::Static) {
            ++statics;
            continue;
        }
        Body b;
        if (!make_body(w, e, c, t.position, polygons, pools, b)) continue;
        b.type = type;
        pw.bodies.push_back(b);
        if (!c.trigger && c.shape == ShapeType::Box) pw.dyn_solid_layers |= c.layer;
    }
    pw.statics_seen = statics;
    pw.dyn.build(pw.bodies, 0u, pw.config.cell_size);
    pw.margin = 0.0f;
    pw.dyn_valid = true;
    pw.dyn_fresh = false;
    pw.dyn_tick = w.tick();
    pw.versions = dyn_stamp(w);
    ++pw.dyn_builds;
    // A body became static (or a static appeared) since the static grid was built.
    if (pw.statics_seen != pw.static_total) {
        pw.static_dirty = true;
        update_static(w, pw, false);
    }
}

bool update_static(World &w, PhysicsWorld &pw, bool verify) {
    const PolygonShapes *polygons = w.resources().try_get<PolygonShapes>();
    const std::size_t polygon_count = polygons != nullptr ? polygons->verts.size() : 0;
    const BodyPools pools = body_pools(w);
    const std::array<std::uint32_t, 7> classes = class_stamp(w);
    // Cached polygon views point into PolygonShapes, which may have reallocated.
    bool rebuild = pw.static_dirty || polygon_count != pw.static_polygons;
    if (!rebuild && verify) {
        // Did a static move, change its Collider, or change type?
        const bool reclassify = classes != pw.class_versions;
        for (std::size_t i = 0; i < pw.statics.size() && !rebuild; ++i) {
            const Entity e = pw.statics[i].entity;
            const Transform *t = w.try_get<Transform>(e);
            const Collider *c = w.try_get<Collider>(e);
            if (t == nullptr || c == nullptr || !(t->position == pw.static_positions[i]) ||
                !same_collider(*c, pw.static_colliders[i])) {
                rebuild = true;
                break;
            }
            const RigidBody *rb = pools.rigid ? w.try_get<RigidBody>(e) : nullptr;
            if (rb != nullptr ? rb->type != BodyType::Static
                              : reclassify && classify(w, e, nullptr, pools) != BodyType::Static) {
                rebuild = true;
            }
        }
    }
    // Colliders were added or removed; recount statics to see if any of them was static.
    const std::uint32_t collider_version = w.pool_version<Collider>();
    if (!rebuild && collider_version != pw.static_collider_version) {
        std::size_t count = 0;
        for (auto [e, t, c] : w.view<Transform, Collider>()) {
            (void)t;
            (void)c;
            const RigidBody *rb = pools.rigid ? w.try_get<RigidBody>(e) : nullptr;
            if (classify(w, e, rb, pools) == BodyType::Static) ++count;
        }
        rebuild = count != pw.static_total;
        pw.static_collider_version = collider_version;
    }
    if (!rebuild) return false;
    pw.statics.clear();
    pw.static_positions.clear();
    pw.static_colliders.clear();
    pw.static_solid_layers = 0;
    std::size_t total = 0;
    for (auto [e, t, c] : w.view<Transform, Collider>()) {
        const RigidBody *rb = pools.rigid ? w.try_get<RigidBody>(e) : nullptr;
        if (classify(w, e, rb, pools) != BodyType::Static) continue;
        ++total;
        Body b;
        if (!make_body(w, e, c, t.position, polygons, pools, b)) continue;
        b.type = BodyType::Static;
        pw.statics.push_back(b);
        pw.static_positions.push_back(t.position);
        pw.static_colliders.push_back(c);
        if (!c.trigger && c.shape == ShapeType::Box) pw.static_solid_layers |= c.layer;
    }
    pw.stat.build(pw.statics, kStaticRef, pw.config.cell_size);
    pw.static_dirty = false;
    pw.static_polygons = polygon_count;
    pw.static_collider_version = collider_version;
    pw.class_versions = classes;
    ++pw.static_builds;
    // A gather counts the statics it meets; a different count means this cache is stale.
    pw.statics_seen = total;
    pw.static_total = total;
    return true;
}

void ensure_grids(World &w, PhysicsWorld &pw) {
    update_static(w, pw, false);
    if (!pw.dyn_valid || dyn_stamp(w) != pw.versions) build_dynamic(w, pw);
}

// ---- Pairs -----------------------------------------------------------------------------------------

void find_pairs(PhysicsWorld &pw) {
    pw.pairs.clear();
    pw.dropped_pairs = 0;
    PairSink sink{&pw, pw.config.pair_capacity};
    const CellGrid &dyn = pw.dyn;
    const CellGrid &stat = pw.stat;
    // Dynamic against dynamic, bucket by bucket.
    for (std::uint32_t bk = 0; bk < dyn.bucket_count() && dyn.entry_count() != 0; ++bk) {
        const std::span<const CellEntry> es = dyn.bucket(bk);
        for (std::size_t i = 0; i < es.size(); ++i) {
            const CellEntry &ei = es[i];
            const Body &a = pw.bodies[ei.ref];
            // An empty layer or mask can't pair with anything, so skip the cell.
            if (a.layer == 0 || a.mask == 0) continue;
            for (std::size_t j = i + 1; j < es.size(); ++j) {
                const CellEntry &ej = es[j];
                if (ej.cx != ei.cx || ej.cy != ei.cy) continue;
                const Body &b = pw.bodies[ej.ref];
                if (!grown_overlap(a, b) || !owns_pair(dyn, a, b, ei.cx, ei.cy)) continue;
                sink.add(ei.ref, ej.ref);
            }
            // Against the statics of the same cell.
            for (const CellEntry &es2 : stat.bucket_for(ei.cx, ei.cy)) {
                if (es2.cx != ei.cx || es2.cy != ei.cy) continue;
                const Body &b = pw.statics[es2.ref & ~kStaticRef];
                if (!grown_overlap(a, b) || !owns_pair(dyn, a, b, ei.cx, ei.cy)) continue;
                sink.add(ei.ref, es2.ref);
            }
        }
    }
    // Oversize bodies against everything they may touch (each pair once).
    const std::span<const std::uint32_t> dyn_big = dyn.oversize();
    const std::span<const std::uint32_t> stat_big = stat.oversize();
    for (const std::uint32_t ref : dyn_big) {
        const Body &a = pw.bodies[ref];
        for (std::uint32_t j = 0; j < pw.bodies.size(); ++j) {
            if (j == ref || (is_oversize(dyn_big, j) && j < ref)) continue;
            if (grown_overlap(a, pw.bodies[j])) sink.add(ref, j);
        }
        for (std::uint32_t j = 0; j < pw.statics.size(); ++j) {
            if (grown_overlap(a, pw.statics[j])) sink.add(ref, kStaticRef | j);
        }
    }
    for (const std::uint32_t sref : stat_big) {
        const Body &s = pw.statics[sref & ~kStaticRef];
        for (std::uint32_t j = 0; j < pw.bodies.size(); ++j) {
            if (is_oversize(dyn_big, j)) continue;
            if (grown_overlap(s, pw.bodies[j])) sink.add(j, sref);
        }
    }
    sort_pairs(pw.pairs, pw.pair_scratch);
}

}
