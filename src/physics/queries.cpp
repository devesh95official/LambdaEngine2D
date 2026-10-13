// Spatial queries. Candidates come from the grids, then get tested at their current Transform,
// so a query never reports a body where it used to be.
#include <algorithm>
#include <cmath>
#include <limits>

#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/queries.h>

#include "detail/physics_world.h"
#include "detail/tile_math.h"

namespace lam {
namespace {

using detail::Body;
using detail::PhysicsWorld;
using detail::ShapeView;

PhysicsWorld *physics_of(World &w) {
    PhysicsWorld *pw = w.resources().try_get<PhysicsWorld>();
    if (pw == nullptr) return nullptr;
    if (pw->dyn_valid && pw->dyn_tick != w.tick()) {
        // First query this tick: game code or a restore may have moved things since Physics ran.
        detail::update_static(w, *pw, true);
        pw->dyn_valid = false;
    }
    detail::ensure_grids(w, *pw);
    return pw;
}

bool live_shape(World &w, Entity e, ShapeView &out, const Collider **collider = nullptr) {
    const Collider *c = w.try_get<Collider>(e);
    const Transform *t = w.try_get<Transform>(e);
    if (c == nullptr || t == nullptr) return false;
    out = detail::make_shape(*c, t->position, w.resources().try_get<PolygonShapes>());
    if (collider != nullptr) *collider = c;
    return is_finite(out.center);
}

// Returns the total, which can be more than out.size().
std::size_t emit_sorted(PhysicsWorld &pw, std::span<Entity> out) {
    std::sort(pw.found.begin(), pw.found.end(), [](Entity a, Entity b) { return a.index < b.index; });
    const std::size_t n = pw.found.size() < out.size() ? pw.found.size() : out.size();
    for (std::size_t i = 0; i < n; ++i) out[i] = pw.found[i];
    return pw.found.size();
}

std::optional<RayHit> sweep_query(World &w, Vec2 from, Vec2 to, float radius, const RayQuery &q) {
    if (!is_finite(from) || !is_finite(to)) return std::nullopt;
    const float r = radius > 0.0f && std::isfinite(radius) ? radius : 0.0f;
    const float length_total = distance(from, to);
    std::optional<RayHit> best;
    float best_t = std::numeric_limits<float>::infinity();
    const Vec2 back = normalize_or(from - to, Vec2{});
    if (const TileWorld *tw = w.resources().try_get<TileWorld>(); tw != nullptr && q.tiles) {
        if (const std::optional<detail::TileSweepHit> h = detail::sweep_circle_tiles(*tw, from, to, r)) {
            best_t = h->t;
            const Vec2 n = h->normal.x == 0.0f && h->normal.y == 0.0f ? back : h->normal;
            best = RayHit{Entity{}, h->point, n, h->t * length_total, true, h->cell};
        }
    }
    PhysicsWorld *pw = physics_of(w);
    if (pw == nullptr) return best;
    const Rect region = Rect::from_min_max(min(from, to), max(from, to)).expanded(r);
    Entity best_entity;
    detail::for_candidates(*pw, region, [&](std::uint32_t ref) {
        const Body &b = detail::body_at(*pw, ref);
        if (q.ignore && b.entity == q.ignore) return;
        ShapeView shape;
        const Collider *c = nullptr;
        if (!live_shape(w, b.entity, shape, &c) || (c->trigger && !q.triggers) || (c->layer & q.mask) == 0) return;
        const std::optional<SegmentHit> h = detail::sweep_shape(shape, from, to, r);
        if (!h) return;
        // Bodies win ties with tiles; between bodies the lower index wins.
        const bool better = h->t < best_t || (h->t == best_t && (!best || best->tile || b.entity.index < best_entity.index));
        if (!better) return;
        best_t = h->t;
        best_entity = b.entity;
        const Vec2 n = h->normal.x == 0.0f && h->normal.y == 0.0f ? back : h->normal;
        best = RayHit{b.entity, h->point, n, h->t * length_total, false, IVec2{}};
    });
    return best;
}

} // namespace

std::optional<RayHit> raycast(World &w, Vec2 origin, Vec2 dir_unit, float max_dist, const RayQuery &q) {
    if (!is_finite(origin) || !is_finite(dir_unit) || !(max_dist >= 0.0f) || !std::isfinite(max_dist)) return std::nullopt;
    const Vec2 dir = normalize_or(dir_unit, Vec2{});
    if (dir.x == 0.0f && dir.y == 0.0f) return std::nullopt;
    return sweep_query(w, origin, origin + dir * max_dist, 0.0f, q);
}

std::optional<RayHit> raycast(World &w, Vec2 origin, Vec2 dir_unit, float max_dist, CollisionMask mask, Entity ignore) {
    return raycast(w, origin, dir_unit, max_dist, RayQuery{mask, ignore, true, false});
}

std::optional<RayHit> sweep_circle(World &w, Vec2 from, Vec2 to, float radius, const RayQuery &q) {
    return sweep_query(w, from, to, radius, q);
}

std::optional<RayHit> sweep_circle(World &w, Vec2 from, Vec2 to, float radius, CollisionMask mask, Entity ignore) {
    return sweep_query(w, from, to, radius, RayQuery{mask, ignore, true, false});
}

std::size_t overlap_box(World &w, Rect r, CollisionMask mask, std::span<Entity> out) {
    PhysicsWorld *pw = physics_of(w);
    if (pw == nullptr || r.empty() || !is_finite(r.min()) || !is_finite(r.size())) return 0;
    ShapeView query;
    query.type = ShapeType::Box;
    query.center = r.center();
    query.half = r.size() * 0.5f;
    query.aabb = r;
    pw->found.clear();
    detail::for_candidates(*pw, r, [&](std::uint32_t ref) {
        const Body &b = detail::body_at(*pw, ref);
        ShapeView shape;
        const Collider *c = nullptr;
        detail::ContactInfo ci;
        if (!live_shape(w, b.entity, shape, &c) || (c->layer & mask) == 0) return;
        if (detail::collide(query, shape, ci, 0.0f)) pw->found.push_back(b.entity);
    });
    return emit_sorted(*pw, out);
}

std::size_t overlap_circle(World &w, Circle c, CollisionMask mask, std::span<Entity> out) {
    PhysicsWorld *pw = physics_of(w);
    if (pw == nullptr || !(c.radius > 0.0f) || !is_finite(c.center) || !std::isfinite(c.radius)) return 0;
    ShapeView query;
    query.type = ShapeType::Circle;
    query.center = c.center;
    query.radius = c.radius;
    query.half = {c.radius, c.radius};
    query.aabb = Rect::from_center(c.center, query.half);
    pw->found.clear();
    detail::for_candidates(*pw, query.aabb, [&](std::uint32_t ref) {
        const Body &b = detail::body_at(*pw, ref);
        ShapeView shape;
        const Collider *c = nullptr;
        detail::ContactInfo ci;
        if (!live_shape(w, b.entity, shape, &c) || (c->layer & mask) == 0) return;
        if (detail::collide(query, shape, ci, 0.0f)) pw->found.push_back(b.entity);
    });
    return emit_sorted(*pw, out);
}

std::size_t query_neighbors(World &w, Vec2 p, float radius, std::span<Entity> out) {
    PhysicsWorld *pw = physics_of(w);
    if (pw == nullptr || !(radius > 0.0f) || !is_finite(p) || !std::isfinite(radius)) return 0;
    pw->found.clear();
    const float r2 = radius * radius;
    detail::for_candidates(*pw, Rect::from_center(p, {radius, radius}), [&](std::uint32_t ref) {
        const Entity e = detail::body_at(*pw, ref).entity;
        const Collider *c = w.try_get<Collider>(e);
        const Transform *t = w.try_get<Transform>(e);
        if (c == nullptr || t == nullptr) return;
        if (distance_sq(t->position + c->offset, p) < r2) pw->found.push_back(e);
    });
    return emit_sorted(*pw, out);
}

Vec2 separation(World &w, Entity self, float radius, float strength, CollisionMask mask) {
    constexpr float kNoDistance = 0.001f;
    if (!(radius > 0.0f) || !(strength > 0.0f) || !std::isfinite(radius) || !std::isfinite(strength)) return {};
    const Transform *st = w.try_get<Transform>(self);
    if (st == nullptr) return {};
    const Collider *sc = w.try_get<Collider>(self);
    const Vec2 me = st->position + (sc != nullptr ? sc->offset : Vec2{});
    PhysicsWorld *pw = physics_of(w);
    if (pw == nullptr || !is_finite(me)) return {};
    Vec2 push;
    int stacked = 0;
    int across = 0;
    const float r2 = radius * radius;
    pw->found.clear();
    // Filter by radius before sorting; most candidates in a crowd are out of range.
    detail::for_candidates(*pw, Rect::from_center(me, {radius, radius}), [&](std::uint32_t ref) {
        const Entity e = detail::body_at(*pw, ref).entity;
        if (e == self) return;
        const Collider *c = w.try_get<Collider>(e);
        const Transform *t = w.try_get<Transform>(e);
        if (c == nullptr || t == nullptr || (c->layer & mask) == 0) return;
        if (!(length_sq(me - (t->position + c->offset)) < r2)) return;
        pw->found.push_back(e);
    });
    // Sum in index order so the float result doesn't depend on the grid layout.
    std::sort(pw->found.begin(), pw->found.end(), [](Entity a, Entity b) { return a.index < b.index; });
    for (const Entity e : pw->found) {
        const Collider &c = w.get<Collider>(e);
        const Vec2 delta = me - (w.get<Transform>(e).position + c.offset);
        const float d2 = length_sq(delta);
        const float d = std::sqrt(d2);
        if (d < kNoDistance) {
            // Same spot: let the indices pick a direction so the stack spreads out.
            ++stacked;
            across += self.index < e.index ? -1 : 1;
            continue;
        }
        push += delta * (strength * (1.0f - d / radius) / d);
    }
    if (stacked > 0) {
        const float x = static_cast<float>(across) / static_cast<float>(stacked);
        const float upright = std::sqrt(std::max(0.0f, 1.0f - x * x));
        push.x += x * strength;
        push.y += (self.index % 2 == 0 ? upright : -upright) * strength;
    }
    return clamp_length(push, strength);
}

Vec2 separation(World &w, Entity self, float radius, float strength) {
    return separation(w, self, radius, strength, 0xFFFFFFFFu);
}

Vec2 separation(std::size_t self, std::span<const Vec2> positions, float radius, float strength) {
    constexpr float kNoDistance = 0.001f;
    if (!(radius > 0.0f) || !(strength > 0.0f) || !std::isfinite(radius) || !std::isfinite(strength) ||
        self >= positions.size()) {
        return {};
    }
    const Vec2 me = positions[self];
    if (!is_finite(me)) return {};
    Vec2 push;
    bool stacked = false;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (i == self) continue;
        const Vec2 delta = me - positions[i];
        const float d = length(delta);
        if (!(d < radius)) continue;
        if (d < kNoDistance) {
            stacked = true;
            continue;
        }
        push += delta * (strength * (1.0f - d / radius) / d);
    }
    if (stacked) {
        // Fan out by index: lowest goes left, highest right, the rest above and below.
        const float last = static_cast<float>(positions.size() - 1);
        const float across = (2.0f * static_cast<float>(self) - last) / last;
        const float upright = std::sqrt(std::max(0.0f, 1.0f - across * across));
        push.x += across * strength;
        push.y += (self % 2 == 0 ? upright : -upright) * strength;
    }
    return clamp_length(push, strength);
}

Entity find_interactable(World &w, Vec2 origin, Vec2 facing_unit, float range, float half_angle, StrId kind) {
    if (!is_finite(origin) || !(range >= 0.0f)) return {};
    const Vec2 facing = normalize_or(facing_unit, Vec2{});
    Entity best;
    float best_gap = std::numeric_limits<float>::infinity();
    for (auto [e, it, t] : w.view<Interactable, Transform>()) {
        if (kind && !(it.kind == kind)) continue;
        const Vec2 at = t.position;
        if (!is_finite(at)) continue;
        const float reach = it.radius > 0.0f && std::isfinite(it.radius) ? it.radius : 0.0f;
        const float d = distance(origin, at);
        const float gap = d - reach;
        if (!(gap <= range)) continue;
        const bool in_cone = d <= reach || (facing.x == 0.0f && facing.y == 0.0f) ||
                             point_in_cone(origin, facing, d + 1.0f, half_angle, at);
        if (!in_cone) continue;
        if (gap < best_gap || (gap == best_gap && e.index < best.index)) {
            best_gap = gap;
            best = e;
        }
    }
    return best;
}

void refresh_queries(World &w) {
    PhysicsWorld *pw = w.resources().try_get<PhysicsWorld>();
    if (pw == nullptr) return;
    detail::update_static(w, *pw, true);
    detail::build_dynamic(w, *pw);
}

TileWorld &ensure_tile_world(World &w) {
    Resources &res = w.resources();
    if (!res.has<TileWorld>()) res.insert<TileWorld>();
    return res.get<TileWorld>();
}

}
