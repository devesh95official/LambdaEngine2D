// Integration, narrowphase and a linear sequential-impulse solver (no rotation).
// Pairs are processed in (a.index, b.index) order so results never depend on memory layout.
#include <algorithm>
#include <cmath>

#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/motors.h>

#include "detail/physics_world.h"
#include "detail/tile_math.h"

namespace lam::detail {
namespace {

constexpr float kBounceThreshold = 20.0f;                       // px/s; slower impacts don't bounce
constexpr float kSlop = 0.01f;                                  // px of penetration left alone
constexpr float kCorrection = 0.8f;                             // share of penetration removed per pass
constexpr float kOneWayTolerance = 0.5f;                        // px of slack for "was above"
constexpr float kProbe = 1.0f / 64.0f;                          // tile contact probe distance

float safe_mass(float m) { return m > 1e-6f && std::isfinite(m) ? m : 1e-6f; }

// Bounce off a tile face if fast enough, otherwise stop.
float bounce(float v, float restitution) {
    const float e = restitution > 0.0f ? (restitution < 1.0f ? restitution : 1.0f) : 0.0f;
    return std::fabs(v) * e > kBounceThreshold * 0.5f ? -v * e : 0.0f;
}

// Coulomb friction: tangential speed drops by at most friction * |dv_normal|.
float rub(float v, float dv_normal, float friction) {
    const float f = friction > 0.0f ? friction : 0.0f;
    const float cut = f * std::fabs(dv_normal);
    if (std::fabs(v) <= cut) return 0.0f;
    return v > 0.0f ? v - cut : v + cut;
}

// Slope tiles are not in the rigid sweep: push the shape out of each one it overlaps.
void resolve_slopes(const TileWorld &tw, const Collider &c, const PolygonShapes *polys, Vec2 &pos, Vec2 &vel,
                    const RigidBody &rb) {
    for (int pass = 0; pass < 2; ++pass) {
        const ShapeView body = make_shape(c, pos, polys);
        const float size = static_cast<float>(tw.tile_size);
        const TileSpan cols = tile_span(body.aabb.x, body.aabb.x + body.aabb.w, tw.origin.x, size, tw.shapes.width());
        const TileSpan rows = tile_span(body.aabb.y, body.aabb.y + body.aabb.h, tw.origin.y, size, tw.shapes.height());
        bool moved = false;
        for (int r = rows.first; r <= rows.last && !moved; ++r) {
            for (int col = cols.first; col <= cols.last && !moved; ++col) {
                const TileShape s = shape_at(tw, IVec2{col, r});
                if (!is_slope(s)) continue;
                const Rect tr = tw.tile_rect(IVec2{col, r});
                Vec2 world[4];
                const int n = tile_polygon(s, tr, world);
                Vec2 local[4];
                const Vec2 centre = tr.center();
                for (int i = 0; i < n; ++i) local[i] = world[i] - centre;
                ShapeView tile;
                tile.type = ShapeType::Polygon;
                tile.center = centre;
                tile.half = tr.size() * 0.5f;
                tile.local = local;
                tile.count = static_cast<std::uint8_t>(n);
                tile.aabb = tr;
                ContactInfo ci;
                if (!collide(body, tile, ci, 0.0f) || !(ci.depth > 0.0f)) continue;
                // Push out of the slope, but never into a Solid tile.
                const Vec2 push = ci.normal * -ci.depth;
                const RigidMove m = move_rigid(tw, body.aabb, push);
                pos += m.box.min() - body.aabb.min();
                const Vec2 n_out = -ci.normal;                  // from the tile toward the body
                const float vn = dot(vel, n_out);
                if (vn < 0.0f) {
                    const Vec2 t = perp(n_out);
                    const float e = rb.restitution;
                    const float new_vn = std::fabs(vn) * e > kBounceThreshold * 0.5f ? -vn * e : 0.0f;
                    const float vt = rub(dot(vel, t), new_vn - vn, rb.friction);
                    vel = n_out * new_vn + t * vt;
                }
                moved = true;
            }
        }
        if (!moved) return;
    }
}

Vec2 kinematic_velocity(const World &w, const Body &b) {
    if (b.type == BodyType::Static) return {};
    const Velocity *v = w.try_get<Velocity>(b.entity);
    return v != nullptr && is_finite(v->linear) ? v->linear : Vec2{};
}

bool awake_dynamic(const Body &b) { return b.type == BodyType::Dynamic && (b.flags & kBodySleeping) == 0; }

std::uint32_t dyn_slot_for(World &w, PhysicsWorld &pw, std::uint32_t ref) {
    if ((ref & kStaticRef) != 0) return kNoRef;
    const Body &b = pw.bodies[ref];
    if (!awake_dynamic(b)) return kNoRef;
    if (pw.dyn_slot[ref] != kNoRef) return pw.dyn_slot[ref];
    DynState s;
    s.ref = ref;
    const Velocity *v = w.try_get<Velocity>(b.entity);
    s.velocity = v != nullptr && is_finite(v->linear) ? v->linear : Vec2{};
    const RigidBody *rb = w.try_get<RigidBody>(b.entity);
    s.inv_mass = 1.0f / safe_mass(rb != nullptr ? rb->mass : 1.0f);
    // Which sides touch Solid tiles (the solver may not push the body into them).
    if (const TileWorld *tw = w.resources().try_get<TileWorld>(); tw != nullptr && has_tiles(*tw) &&
                                                                 (b.flags & kBodyTrigger) == 0) {
        const Rect box = b.shape.aabb;
        // Most bodies are nowhere near a tile, so one probe of the grown box usually settles it.
        if (rigid_blocked(*tw, box.expanded(kProbe))) {
            if (rigid_blocked(*tw, box.translated({-kProbe, 0.0f}))) s.tile_block |= 1;
            if (rigid_blocked(*tw, box.translated({kProbe, 0.0f}))) s.tile_block |= 2;
            if (rigid_blocked(*tw, box.translated({0.0f, -kProbe}))) s.tile_block |= 4;
            if (rigid_blocked(*tw, box.translated({0.0f, kProbe}))) s.tile_block |= 8;
        }
    }
    pw.dyn_slot[ref] = static_cast<std::uint32_t>(pw.dyn_state.size());
    pw.dyn_state.push_back(s);
    return pw.dyn_slot[ref];
}

bool tile_blocks(const DynState &s, Vec2 dir) {
    if (s.tile_block == 0) return false;
    return (dir.x < -0.5f && (s.tile_block & 1) != 0) || (dir.x > 0.5f && (s.tile_block & 2) != 0) ||
           (dir.y < -0.5f && (s.tile_block & 4) != 0) || (dir.y > 0.5f && (s.tile_block & 8) != 0);
}

// A side that would be pushed into a tile it rests on acts immovable.
void contact_masses(const PhysicsWorld &pw, const SolverContact &c, float &ima, float &imb) {
    ima = 0.0f;
    imb = 0.0f;
    if (c.da != kNoRef && !tile_blocks(pw.dyn_state[c.da], -c.normal)) ima = pw.dyn_state[c.da].inv_mass;
    if (c.db != kNoRef && !tile_blocks(pw.dyn_state[c.db], c.normal)) imb = pw.dyn_state[c.db].inv_mass;
}

void clamp_to_tiles(DynState &s) {
    if ((s.tile_block & 1) != 0 && s.velocity.x < 0.0f) s.velocity.x = 0.0f;
    if ((s.tile_block & 2) != 0 && s.velocity.x > 0.0f) s.velocity.x = 0.0f;
    if ((s.tile_block & 4) != 0 && s.velocity.y < 0.0f) s.velocity.y = 0.0f;
    if ((s.tile_block & 8) != 0 && s.velocity.y > 0.0f) s.velocity.y = 0.0f;
}

bool record_less(const ContactRecord &a, const ContactRecord &b) {
    if (a.first.index != b.first.index) return a.first.index < b.first.index;
    if (a.second.index != b.second.index) return a.second.index < b.second.index;
    return a.kind < b.kind;
}

// Grid state for the sub-stepped moves of one integrate pass.
struct SolidScan {
    bool ready = false;                                         // dynamic grid is up to date
    bool built = false;                                         // rebuilt by this pass
    float since = 0.0f;                                         // largest move since the rebuild
};

// Fast bodies (moving more than their smaller half extent) are sub-stepped against static and
// kinematic colliders and stop at the first overlap, so they can't skip over a thin platform.
Vec2 stop_at_solids(World &w, PhysicsWorld &pw, SolidScan &scan, Entity self, const Collider &c,
                    const PolygonShapes *polys, Vec2 from, Vec2 to) {
    const ShapeView start = make_shape(c, from, polys);
    const float step = std::min(start.half.x, start.half.y);
    const Vec2 d = to - from;
    const float reach = std::max(std::fabs(d.x), std::fabs(d.y));
    if (!(step > 0.0f) || !(reach > step) || !is_finite(d)) return to;
    if (!scan.ready) {
        const std::uint64_t builds = pw.dyn_builds;
        if (!pw.dyn_valid || pw.dyn_tick != w.tick()) {
            build_dynamic(w, pw);
        } else {
            ensure_grids(w, pw);
        }
        if (pw.dyn_builds != builds) {
            scan.built = true;
            scan.since = 0.0f;
        }
        scan.ready = true;
    }
    const Rect region = Rect::from_min_max(min(start.aabb.min(), start.aabb.min() + d),
                                           max(start.aabb.max(), start.aabb.max() + d))
                            .expanded(scan.since);
    pw.found.clear();
    for_candidates(pw, region, [&](std::uint32_t ref) {
        const Body &b = body_at(pw, ref);
        if (b.entity == self || b.type == BodyType::Dynamic || (b.flags & kBodyTrigger) != 0) return;
        pw.found.push_back(b.entity);
    });
    std::size_t n = 0;
    for (std::size_t i = 0; i < pw.found.size(); ++i) {
        const Entity o = pw.found[i];
        const Collider *oc = w.try_get<Collider>(o);
        const Transform *ot = w.try_get<Transform>(o);
        if (oc == nullptr || ot == nullptr || oc->trigger) continue;
        if ((c.layer & oc->mask) == 0 || (oc->layer & c.mask) == 0) continue;
        const ShapeView os = make_shape(*oc, ot->position, polys);
        ContactInfo ci;
        if (collide(start, os, ci, 0.0f)) continue;
        if (oc->one_way && !(d.y > 0.0f && start.aabb.y + start.aabb.h <= os.aabb.y + kOneWayTolerance)) continue;
        pw.found[n++] = o;
    }
    pw.found.resize(n);
    if (n == 0) return to;
    const int steps = std::min(64, static_cast<int>(std::ceil(reach / step)));
    for (int k = 1; k <= steps; ++k) {
        const Vec2 p = k == steps ? to : from + d * (static_cast<float>(k) / static_cast<float>(steps));
        const ShapeView at = make_shape(c, p, polys);
        for (const Entity o : pw.found) {
            const ShapeView os = make_shape(w.get<Collider>(o), w.get<Transform>(o).position, polys);
            ContactInfo ci;
            if (collide(at, os, ci, 0.0f)) return p;
        }
    }
    return to;
}

} // namespace

void integrate_bodies(World &w, PhysicsWorld &pw, Vec2 gravity, float dt) {
    const TileWorld *tw = w.resources().try_get<TileWorld>();
    const bool tiles = tw != nullptr && has_tiles(*tw);
    const PolygonShapes *polys = w.resources().try_get<PolygonShapes>();
    if (!is_finite(gravity)) gravity = {};
    float max_move = 0.0f;
    SolidScan scan;
    for (auto [e, v, t] : w.view<Velocity, Transform>(
             Exclude<PlatformerMotor, TopDownMotor, PathFollower, MovingPlatform, Projectile>{})) {
        RigidBody *rb = w.try_get<RigidBody>(e);
        if (rb != nullptr && rb->type == BodyType::Static) continue;
        Vec2 vel = is_finite(v.linear) ? v.linear : Vec2{};
        const Vec2 before = t.position;
        if (rb != nullptr && rb->type == BodyType::Dynamic) {
            if (rb->sleeping) {
                v.linear = vel;
                continue;
            }
            vel += gravity * (std::isfinite(rb->gravity_scale) ? rb->gravity_scale : 0.0f) * dt;
            if (rb->linear_damping > 0.0f && std::isfinite(rb->linear_damping)) vel = vel * (1.0f / (1.0f + rb->linear_damping * dt));
            const Collider *c = w.try_get<Collider>(e);
            if (c != nullptr && !c->trigger && tiles) {
                const ShapeView shape = make_shape(*c, t.position, polys);
                const Rect box = shape.aabb;
                const RigidMove m = move_rigid(*tw, box, vel * dt);
                if (m.started_blocked) {
                    // Spawned or pushed inside a wall: out to the nearest free spot, at rest.
                    if (const std::optional<Vec2> spot = nearest_free_spot(*tw, box, 2)) t.position += *spot - box.min();
                    vel = {};
                } else {
                    const Vec2 tile_end = t.position + (m.box.min() - box.min());
                    const Vec2 end = stop_at_solids(w, pw, scan, e, *c, polys, t.position, tile_end);
                    t.position = end;
                    // Stopped by a collider first: it never reached the tile it would have hit.
                    if (end == tile_end) {
                        if (m.hit_y) {
                            const float vy = bounce(vel.y, rb->restitution);
                            vel.x = rub(vel.x, vy - vel.y, rb->friction);
                            vel.y = vy;
                        }
                        if (m.hit_x) {
                            const float vx = bounce(vel.x, rb->restitution);
                            vel.y = rub(vel.y, vx - vel.x, rb->friction);
                            vel.x = vx;
                        }
                    }
                    resolve_slopes(*tw, *c, polys, t.position, vel, *rb);
                }
            } else if (c != nullptr && !c->trigger) {
                t.position = stop_at_solids(w, pw, scan, e, *c, polys, t.position, t.position + vel * dt);
            } else {
                t.position += vel * dt;
            }
        } else {
            t.position += vel * dt;
        }
        v.linear = vel;
        const float moved = length(t.position - before);
        if (moved > max_move) max_move = moved;
        if (moved > scan.since) scan.since = moved;
    }
    // A rebuild in this pass reset the margin: only the moves after it count.
    if (scan.built) {
        if (scan.since > pw.margin) pw.margin = scan.since;
    } else {
        pw.margin += max_move;
    }
}

void narrowphase(World &w, PhysicsWorld &pw, bool solve) {
    pw.contacts.clear();
    pw.solver.clear();
    pw.dyn_state.clear();
    pw.dyn_slot.assign(pw.bodies.size(), kNoRef);
    for (const Pair &p : pw.pairs) {
        Body &a = (p.a & kStaticRef) != 0 ? pw.statics[p.a & ~kStaticRef] : pw.bodies[p.a];
        Body &b = (p.b & kStaticRef) != 0 ? pw.statics[p.b & ~kStaticRef] : pw.bodies[p.b];
        ContactInfo ci;
        if (!collide(a.shape, b.shape, ci, kContactSkin)) continue;
        const bool ta = (a.flags & kBodyTrigger) != 0;
        const bool tb = (b.flags & kBodyTrigger) != 0;
        if (ta || tb) {
            if (ta) pw.contacts.push_back(ContactRecord{a.entity, b.entity, {}, 1});
            if (tb) pw.contacts.push_back(ContactRecord{b.entity, a.entity, {}, 1});
            continue;
        }
        // One-way colliders hold only bodies that come from above.
        const bool owa = (a.flags & kBodyOneWay) != 0;
        const bool owb = (b.flags & kBodyOneWay) != 0;
        if (owa != owb) {
            const Body &plat = owa ? a : b;
            const Body &other = owa ? b : a;
            const Transform *ot = w.try_get<Transform>(other.entity);
            const PrevTransform *op = w.try_get<PrevTransform>(other.entity);
            const Transform *pt = w.try_get<Transform>(plat.entity);
            const PrevTransform *pp = w.try_get<PrevTransform>(plat.entity);
            const float other_drop = ot != nullptr && op != nullptr ? ot->position.y - op->position.y : 0.0f;
            const float plat_drop = pt != nullptr && pp != nullptr ? pt->position.y - pp->position.y : 0.0f;
            const float was_bottom = other.shape.aabb.y + other.shape.aabb.h - other_drop;
            const float was_top = plat.shape.aabb.y - plat_drop;
            if (was_bottom > was_top + kOneWayTolerance) continue;
            // Came from above: push up even if the sideways overlap is shallower (landing on an end).
            ci.normal = owa ? Vec2{0.0f, -1.0f} : Vec2{0.0f, 1.0f};
            ci.depth = other.shape.aabb.y + other.shape.aabb.h - plat.shape.aabb.y;
        }
        pw.contacts.push_back(ContactRecord{a.entity, b.entity, ci.normal, 0});
        if (!solve) continue;
        // Sleeping bodies wake when something moving touches them.
        for (int side = 0; side < 2; ++side) {
            Body &s = side == 0 ? a : b;
            const Body &o = side == 0 ? b : a;
            if (s.type != BodyType::Dynamic || (s.flags & kBodySleeping) == 0) continue;
            const bool moving = awake_dynamic(o) || length_sq(kinematic_velocity(w, o)) > 0.0f;
            if (!moving) continue;
            if (RigidBody *rb = w.try_get<RigidBody>(s.entity)) rb->sleeping = false;
            s.flags = static_cast<std::uint16_t>(s.flags & ~kBodySleeping);
        }
        if (!awake_dynamic(a) && !awake_dynamic(b)) continue;
        SolverContact sc;
        sc.a = p.a;
        sc.b = p.b;
        sc.da = dyn_slot_for(w, pw, p.a);
        sc.db = dyn_slot_for(w, pw, p.b);
        sc.normal = ci.normal;
        sc.depth = ci.depth;
        const RigidBody *ra = w.try_get<RigidBody>(a.entity);
        const RigidBody *rb = w.try_get<RigidBody>(b.entity);
        const RigidBody def;
        const RigidBody &ma = ra != nullptr ? *ra : def;
        const RigidBody &mb = rb != nullptr ? *rb : def;
        sc.restitution = std::max(ma.restitution, mb.restitution);
        const float fa = ma.friction > 0.0f ? ma.friction : 0.0f;
        const float fb = mb.friction > 0.0f ? mb.friction : 0.0f;
        sc.friction = std::sqrt(fa * fb);
        const Vec2 va = sc.da != kNoRef ? pw.dyn_state[sc.da].velocity : kinematic_velocity(w, a);
        const Vec2 vb = sc.db != kNoRef ? pw.dyn_state[sc.db].velocity : kinematic_velocity(w, b);
        const float vn = dot(vb - va, sc.normal);
        sc.bounce = vn < -kBounceThreshold ? -vn * std::clamp(sc.restitution, 0.0f, 1.0f) : 0.0f;
        pw.solver.push_back(sc);
    }
    std::sort(pw.contacts.begin(), pw.contacts.end(), record_less);
}

void solve_contacts(World &w, PhysicsWorld &pw, float dt) {
    (void)dt;
    if (pw.solver.empty()) return;
    // Masses and immovable-side velocities don't change during the solve; compute them once.
    for (SolverContact &c : pw.solver) {
        contact_masses(pw, c, c.ima, c.imb);
        c.kva = c.da != kNoRef ? Vec2{} : kinematic_velocity(w, body_at(pw, c.a));
        c.kvb = c.db != kNoRef ? Vec2{} : kinematic_velocity(w, body_at(pw, c.b));
    }
    for (int it = 0; it < pw.config.solver_iterations; ++it) {
        for (SolverContact &c : pw.solver) {
            const float ima = c.ima;
            const float imb = c.imb;
            const float k = ima + imb;
            if (!(k > 0.0f)) continue;
            Vec2 va = c.da != kNoRef ? pw.dyn_state[c.da].velocity : c.kva;
            Vec2 vb = c.db != kNoRef ? pw.dyn_state[c.db].velocity : c.kvb;
            const Vec2 n = c.normal;
            float vn = dot(vb - va, n);
            float jn = (c.bounce - vn) / k;
            const float acc = std::max(c.normal_impulse + jn, 0.0f);
            jn = acc - c.normal_impulse;
            c.normal_impulse = acc;
            va -= n * (jn * ima);
            vb += n * (jn * imb);
            const Vec2 t = perp(n);
            const float vt = dot(vb - va, t);
            float jt = -vt / k;
            const float limit = c.friction * c.normal_impulse;
            const float tacc = std::clamp(c.tangent_impulse + jt, -limit, limit);
            jt = tacc - c.tangent_impulse;
            c.tangent_impulse = tacc;
            va -= t * (jt * ima);
            vb += t * (jt * imb);
            if (c.da != kNoRef) {
                pw.dyn_state[c.da].velocity = va;
                clamp_to_tiles(pw.dyn_state[c.da]);
            }
            if (c.db != kNoRef) {
                pw.dyn_state[c.db].velocity = vb;
                clamp_to_tiles(pw.dyn_state[c.db]);
            }
            (void)vn;
        }
    }
    // Position projection. Twice as many passes as velocity so corrections reach the top of a stack.
    for (int it = 0; it < pw.config.solver_iterations * 2; ++it) {
        for (const SolverContact &c : pw.solver) {
            const float ima = c.ima;
            const float imb = c.imb;
            const float k = ima + imb;
            if (!(k > 0.0f)) continue;
            const Vec2 ca = c.da != kNoRef ? pw.dyn_state[c.da].correction : Vec2{};
            const Vec2 cb = c.db != kNoRef ? pw.dyn_state[c.db].correction : Vec2{};
            const float sep = c.depth - dot(cb - ca, c.normal);
            const float push = std::max(sep - kSlop, 0.0f) * kCorrection / k;
            if (!(push > 0.0f)) continue;
            if (c.da != kNoRef) pw.dyn_state[c.da].correction -= c.normal * (push * ima);
            if (c.db != kNoRef) pw.dyn_state[c.db].correction += c.normal * (push * imb);
        }
    }
    const TileWorld *tw = w.resources().try_get<TileWorld>();
    const bool tiles = tw != nullptr && has_tiles(*tw);
    float max_move = 0.0f;
    for (const DynState &s : pw.dyn_state) {
        const Body &b = pw.bodies[s.ref];
        Transform *t = w.try_get<Transform>(b.entity);
        Velocity *v = w.try_get<Velocity>(b.entity);
        if (v != nullptr) v->linear = s.velocity;
        if (t == nullptr || !is_finite(s.correction)) continue;
        Vec2 delta = s.correction;
        if (tiles && (b.flags & kBodyTrigger) == 0) {
            const RigidMove m = move_rigid(*tw, b.shape.aabb, delta);
            delta = m.box.min() - b.shape.aabb.min();
        }
        t->position += delta;
        const float moved = length(delta);
        if (moved > max_move) max_move = moved;
    }
    pw.margin += max_move;
}

}
