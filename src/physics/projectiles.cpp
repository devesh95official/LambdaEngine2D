// Projectile sweeps. Hits are ordered by contact time, then entity index. Anything already
// overlapped at the start of the tick was hit earlier and is skipped, except on the first tick
// so point-blank shots land.
#include <algorithm>
#include <cmath>
#include <limits>

#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>

#include "detail/physics_world.h"
#include "detail/tile_math.h"

namespace lam::detail {

void run_projectiles(World &w, PhysicsWorld &pw, float dt) {
    pw.projectile_hits.clear();
    if (w.count<Projectile>() == 0) return;
    build_dynamic(w, pw);
    pw.dyn_fresh = true;
    const TileWorld *tw = w.resources().try_get<TileWorld>();
    const PolygonShapes *polys = w.resources().try_get<PolygonShapes>();
    bool body_moved = false;
    for (auto [e, p, t, v] : w.view<Projectile, Transform, Velocity>()) {
        const Vec2 from = t.position;
        if (!is_finite(from) || !is_finite(v.linear)) continue;
        const bool first_tick = w.has<JustSpawned>(e);
        // At rest, nothing to report.
        if (v.linear.x == 0.0f && v.linear.y == 0.0f && !first_tick) continue;
        const Vec2 to = from + v.linear * dt;
        const float radius = p.radius > 0.0f && std::isfinite(p.radius) ? p.radius : 0.0f;
        const Entity self = e;
        const Entity owner = p.owner;
        const CollisionMask mask = p.mask;

        float tile_t = std::numeric_limits<float>::infinity();
        TileSweepHit tile_hit;
        if (p.stop_at_tiles && tw != nullptr) {
            if (const std::optional<TileSweepHit> h = sweep_circle_tiles(*tw, from, to, radius, !first_tick)) {
                tile_t = h->t;
                tile_hit = *h;
            }
        }

        pw.hits.clear();
        const Rect region = Rect::from_min_max(min(from, to), max(from, to)).expanded(radius);
        for_candidates(pw, region, [&](std::uint32_t ref) {
            const Body &b = body_at(pw, ref);
            const Entity target = b.entity;
            if (target == self || (owner && target == owner)) return;
            const Collider *c = w.try_get<Collider>(target);
            const Transform *tt = w.try_get<Transform>(target);
            if (c == nullptr || tt == nullptr || c->trigger || (c->layer & mask) == 0) return;
            const ShapeView shape = make_shape(*c, tt->position, polys);
            const std::optional<SegmentHit> h = sweep_shape(shape, from, to, radius);
            if (!h) return;
            const bool started_inside = h->t == 0.0f && h->normal.x == 0.0f && h->normal.y == 0.0f;
            if (started_inside && !first_tick) {
                // Already hit last tick unless the body moved onto us since then.
                const PrevTransform *prev = w.try_get<PrevTransform>(target);
                const ShapeView before = prev != nullptr ? make_shape(*c, prev->position, polys) : shape;
                if (point_near_shape(before, from, radius)) return;
            }
            Vec2 normal = h->normal;
            if (started_inside) normal = normalize_or(from - to, Vec2{});
            pw.hits.push_back(PhysicsWorld::Hit{h->t, target, h->point, normal});
        });
        std::sort(pw.hits.begin(), pw.hits.end(), [](const PhysicsWorld::Hit &a, const PhysicsWorld::Hit &b) {
            return a.t != b.t ? a.t < b.t : a.entity.index < b.entity.index;
        });

        bool stopped = false;
        Vec2 end = to;
        for (const PhysicsWorld::Hit &h : pw.hits) {
            if (h.t > tile_t) break;
            pw.projectile_hits.push_back(ProjectileHit{self, h.entity, h.point, h.normal, false});
            if (p.pierce > 0) {
                --p.pierce;
                continue;
            }
            end = h.point;
            stopped = true;
            break;
        }
        if (!stopped && tile_t <= 1.0f) {
            Vec2 normal = tile_hit.normal;
            if (normal.x == 0.0f && normal.y == 0.0f) normal = normalize_or(from - to, Vec2{});
            pw.projectile_hits.push_back(ProjectileHit{self, Entity{}, tile_hit.point, normal, true});
            end = tile_hit.point;
            stopped = true;
        }
        t.position = end;
        if (stopped) v.linear = Vec2{};
        if (w.has<Collider>(e)) {
            // Also a body: grow the margin so later sweeps this pass still find it.
            const float moved = length(end - from);
            if (moved > pw.margin) pw.margin = moved;
            if (moved > 0.0f) body_moved = true;
        }
    }
    if (body_moved) pw.dyn_fresh = false;
}

}
