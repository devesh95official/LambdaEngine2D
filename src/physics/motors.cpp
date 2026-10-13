// Motors, path followers, moving platforms and move_body. Riders are carried by the platform's
// last_delta before the motors run. Solids are sorted by index so ties don't depend on the grid.
#include <algorithm>
#include <cmath>
#include <limits>

#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/motors.h>
#include <lam/physics/queries.h>

#include "detail/physics_world.h"
#include "detail/tile_math.h"

namespace lam {

Vec2 step_velocity(Vec2 v, Vec2 input, float max_speed, float accel, float decel, float dt) {
    const bool held = input.x != 0.0f || input.y != 0.0f;
    const Vec2 wanted = input * max_speed;
    const float reach = (held ? accel : decel) * dt;
    const Vec2 delta = wanted - v;
    const float dist = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    // Return `wanted` directly: v + delta can be off by a rounding step and a released stick
    // has to reach exactly zero.
    if (!(dist > reach)) return wanted;
    const float share = reach / dist;
    return Vec2{v.x + delta.x * share, v.y + delta.y * share};
}

namespace detail {
namespace {

const TileWorld &empty_tiles() {
    static const TileWorld kEmpty{};
    return kEmpty;
}

const TileWorld &tiles_of(World &w) {
    const TileWorld *tw = w.resources().try_get<TileWorld>();
    return tw != nullptr ? *tw : empty_tiles();
}

float finite_or(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

float approach(float from, float to, float step) {
    if (!(step > 0.0f)) return from;
    if (from < to) return from + step < to ? from + step : to;
    if (from > to) return from - step > to ? from - step : to;
    return to;
}

Rect swept(const Rect &box, Vec2 delta) {
    const Rect moved = box.translated(delta);
    return Rect::from_min_max(min(box.min(), moved.min()), max(box.max(), moved.max()));
}

// Swept box plus the head room a step-up can lift it into, so move_box sees every solid it
// might meet.
Rect platformer_region(const Rect &box, Vec2 delta, float margin) {
    Rect r = swept(box, delta);
    const float lift = std::min(box.w * 0.5f, box.h * 0.75f) + std::fabs(delta.x);
    if (lift > 0.0f) {
        r.y -= lift;
        r.h += lift;
    }
    return r.expanded(margin);
}

// Riders sunk this far into a platform top get put back on it.
constexpr float kOnTopTolerance = 1.0f / 64.0f;

// Nudge the rider up until its box is flush on `top`. Rounding can leave it a few ulps inside,
// and move_box ignores solids a box already overlaps, so it would fall through.
void settle_on(Transform &t, const Collider &c, float top) {
    const Rect r = collider_box(c, t.position);
    const float over = r.y + r.h - top;
    if (!(over > 0.0f)) return;
    t.position.y -= over;
    for (int i = 0; i < 64; ++i) {
        const Rect now = collider_box(c, t.position);
        if (!(now.y + now.h > top)) return;
        t.position.y = std::nextafter(t.position.y, -std::numeric_limits<float>::infinity());
    }
}

// Solids first, then one-ways.
Rect &gathered_rect(PhysicsWorld &pw, std::size_t i) {
    const std::size_t n = pw.solid_rects.size();
    return i < n ? pw.solid_rects[i] : pw.one_way_rects[i - n];
}

void platformer_step(World &w, PhysicsWorld &pw, const TileWorld &tw, Entity e, PlatformerMotor &m, Transform &t,
                     const Collider &c, Velocity &vel, float dt) {
    Vec2 v = is_finite(vel.linear) ? vel.linear : Vec2{};
    const float move_x = clamp(finite_or(m.move_x, 0.0f), -1.0f, 1.0f);
    const float climb = clamp(finite_or(m.climb_y, 0.0f), -1.0f, 1.0f);
    const Rect box = collider_box(c, t.position);
    if (!is_finite(box.min()) || !is_finite(box.size())) return;

    // Ladders: grab one by climbing while overlapping it; let go by leaving it or jumping.
    IVec2 cell;
    const bool ladder_here = find_flag(tw, box, kTileLadder, cell);
    if (!ladder_here) {
        m.on_ladder = false;
    } else if (!m.on_ladder && climb != 0.0f && !m.jump_pressed) {
        m.on_ladder = true;
    }

    // Jump buffer and coyote time, both counted in ticks.
    if (m.jump_pressed) m.buffer_left = m.buffer_ticks > 0 ? m.buffer_ticks : std::uint8_t{1};
    const bool can_jump = m.on_ground || m.coyote_left > 0 || m.on_ladder;
    if (!m.on_ground && m.coyote_left > 0) --m.coyote_left;
    bool jumped = false;
    if (m.buffer_left > 0 && can_jump) {
        v.y = -m.jump_speed;
        m.buffer_left = 0;
        m.coyote_left = 0;
        m.on_ladder = false;
        jumped = true;
    }

    if (m.on_ladder) {
        v = Vec2{move_x * m.climb_speed, climb * m.climb_speed};
    } else {
        const float target = move_x * m.run_speed;
        float rate = move_x != 0.0f ? m.accel : m.decel;
        if (!m.on_ground) rate *= m.air_control;
        v.x = approach(v.x, target, rate * dt);
        v.y += m.gravity * dt;
        if (m.max_fall > 0.0f && v.y > m.max_fall) v.y = m.max_fall;
        // Jump cut: letting go early caps the rise.
        if (!m.jump_held && v.y < 0.0f && v.y >= -m.jump_speed) {
            const float cap = -m.jump_speed * m.jump_cut;
            if (v.y < cap) v.y = cap;
        }
    }

    const Vec2 delta = v * dt;
    gather_solids(w, pw, e, platformer_region(box, delta, 1.0f + std::max(m.snap_down, 0.0f)), m.solid_mask, true);
    MoveBoxOptions o;
    o.platformer = true;
    o.drop_through = m.drop || (m.on_ladder && climb > 0.0f);
    o.snap_down = m.on_ground && !jumped && !m.on_ladder ? m.snap_down : 0.0f;
    o.solids = pw.solid_rects;
    o.one_way = pw.one_way_rects;
    const BoxMove r = move_box(tw, box, delta, o);
    const Vec2 moved = r.box.min() - box.min();
    t.position += moved;
    const float dist = length(moved);
    if (dist > pw.margin) pw.margin = dist;

    m.on_ground = r.grounded;
    if (r.grounded && v.y > 0.0f) v.y = 0.0f;
    m.on_ceiling = r.hit_y && delta.y < 0.0f;
    if (m.on_ceiling && v.y < 0.0f) v.y = 0.0f;
    m.on_wall_left = r.hit_x && delta.x < 0.0f;
    m.on_wall_right = r.hit_x && delta.x > 0.0f;
    if (r.hit_x) v.x = 0.0f;
    m.on_slope = r.grounded && r.on_slope;
    m.ground_normal = r.grounded ? r.ground_normal : Vec2{0.0f, -1.0f};
    m.riding = Entity{};
    if (r.grounded && r.ground_solid >= 0 && static_cast<std::size_t>(r.ground_solid) < pw.solid_entities.size()) {
        const Entity g = pw.solid_entities[static_cast<std::size_t>(r.ground_solid)];
        if (w.has<MovingPlatform>(g)) m.riding = g;
    }
    if (m.on_ground) m.coyote_left = m.coyote_ticks;
    if (!jumped && m.buffer_left > 0) --m.buffer_left;
    vel.linear = v;
}

void top_down_step(World &w, PhysicsWorld &pw, const TileWorld &tw, Entity e, TopDownMotor &m, Transform &t,
                   const Collider &c, Velocity &vel, float dt) {
    const Vec2 wish = clamp_input(m.move);
    Vec2 v = is_finite(vel.linear) ? vel.linear : Vec2{};
    v = step_velocity_walled(v, wish, m.max_speed, m.accel, m.decel, dt, m.wall);
    const Rect box = collider_box(c, t.position);
    if (!is_finite(box.min()) || !is_finite(box.size())) return;
    const Vec2 delta = v * dt;
    gather_solids(w, pw, e, swept(box, delta).expanded(1.0f + std::max(m.corner_assist, 0.0f)), m.solid_mask, false);
    MoveBoxOptions o;
    o.corner_assist = m.corner_assist;
    o.solids = pw.solid_rects;
    const BoxMove r = move_box(tw, box, delta, o);
    const Vec2 moved = r.box.min() - box.min();
    t.position += moved;
    const float dist = length(moved);
    if (dist > pw.margin) pw.margin = dist;
    m.wall.x = r.hit_x ? (v.x < 0.0f ? -1 : 1) : 0;
    m.wall.y = r.hit_y ? (v.y < 0.0f ? -1 : 1) : 0;
    // Don't let speed build up against a wall or the body shoots off the end of it.
    if (r.hit_x) v.x = 0.0f;
    if (r.hit_y) v.y = 0.0f;
    vel.linear = v;
}

} // namespace

Vec2 clamp_input(Vec2 input) {
    if (!is_finite(input)) return Vec2{};
    const float size = std::sqrt(input.x * input.x + input.y * input.y);
    return size > 1.0f ? Vec2{input.x / size, input.y / size} : input;
}

Vec2 step_velocity_walled(Vec2 v, Vec2 input, float max_speed, float accel, float decel, float dt, IVec2 wall) {
    const bool leaning_x = (wall.x < 0 && input.x < 0.0f) || (wall.x > 0 && input.x > 0.0f);
    const bool leaning_y = (wall.y < 0 && input.y < 0.0f) || (wall.y > 0 && input.y > 0.0f);
    if (!leaning_x && !leaning_y) return step_velocity(v, input, max_speed, accel, decel, dt);
    const float reach = accel * dt;
    const auto toward = [reach](float from, float wanted) {
        const float delta = wanted - from;
        if (!(std::fabs(delta) > reach)) return wanted;
        return from + (delta < 0.0f ? -reach : reach);
    };
    return Vec2{toward(leaning_x ? 0.0f : v.x, input.x * max_speed), toward(leaning_y ? 0.0f : v.y, input.y * max_speed)};
}

void gather_solids(World &w, PhysicsWorld &pw, Entity self, const Rect &region, CollisionMask solid_mask,
                   bool platformer) {
    pw.solid_rects.clear();
    pw.one_way_rects.clear();
    pw.solid_entities.clear();
    pw.found.clear();
    if (solid_mask == 0) return;                                // tiles only
    // Nothing on a matching layer, skip the query.
    const CollisionMask solid_layers = pw.dyn_solid_layers | pw.static_solid_layers;
    if (pw.solid_layers_fresh && (solid_layers & solid_mask) == 0) return;
    // Filter first and sort only the solids; crowds have lots of candidates but few solids.
    for_candidates(pw, region, [&](std::uint32_t ref) {
        const Entity e = body_at(pw, ref).entity;
        if (e == self) return;
        const Collider *c = w.try_get<Collider>(e);
        const Transform *t = w.try_get<Transform>(e);
        if (c == nullptr || t == nullptr || c->trigger || c->shape != ShapeType::Box) return;
        if ((c->layer & solid_mask) == 0 || (c->one_way && !platformer)) return;
        if (!collider_box(*c, t->position).overlaps(region)) return;
        pw.found.push_back(e);
    });
    std::sort(pw.found.begin(), pw.found.end(), [](Entity a, Entity b) { return a.index < b.index; });
    for (int pass = 0; pass < 2; ++pass) {
        for (const Entity e : pw.found) {
            const Collider &c = w.get<Collider>(e);
            if (c.one_way != (pass == 1)) continue;
            (pass == 0 ? pw.solid_rects : pw.one_way_rects).push_back(collider_box(c, w.get<Transform>(e).position));
            pw.solid_entities.push_back(e);
        }
    }
}

void run_path_followers(World &w, PhysicsWorld &pw, float dt) {
    float max_move = 0.0f;
    for (auto [e, pf, t] : w.view<PathFollower, Transform>()) {
        const int count = pf.count < pf.points.size() ? pf.count : static_cast<int>(pf.points.size());
        const Vec2 start = t.position;
        Vec2 pos = start;
        if (count >= 2 && is_finite(pos)) {
            if (pf.next >= count) pf.next = static_cast<std::uint8_t>(count - 1);
            float remaining = pf.speed > 0.0f && std::isfinite(pf.speed) ? pf.speed * dt : 0.0f;
            for (int guard = 0; guard < 2 * count + 2 && remaining > 0.0f; ++guard) {
                const Vec2 target = pf.points[pf.next];
                if (!is_finite(target)) break;
                const float d = distance(pos, target);
                if (d > remaining) {
                    pos += (target - pos) * (remaining / d);
                    remaining = 0.0f;
                    break;
                }
                pos = target;
                remaining -= d;
                if (pf.forward) {
                    if (pf.next + 1 < count) {
                        ++pf.next;
                    } else if (pf.ping_pong) {
                        pf.forward = false;
                        pf.next = static_cast<std::uint8_t>(count - 2);
                    } else {
                        pf.next = 0;
                    }
                } else {
                    if (pf.next > 0) {
                        --pf.next;
                    } else if (pf.ping_pong) {
                        pf.forward = true;
                        pf.next = 1;
                    } else {
                        pf.next = static_cast<std::uint8_t>(count - 1);
                    }
                }
                if (d == 0.0f && remaining == 0.0f) break;
            }
            t.position = pos;
        }
        if (Velocity *v = w.try_get<Velocity>(e)) v->linear = dt > 0.0f ? (pos - start) / dt : Vec2{};
        const float moved = length(pos - start);
        if (moved > max_move) max_move = moved;
    }
    pw.margin += max_move;
}

void run_moving_platforms(World &w, PhysicsWorld &pw, float dt) {
    float max_move = 0.0f;
    for (auto [e, mp, t] : w.view<MovingPlatform, Transform>()) {
        const Vec2 before = t.position;
        if (!w.has<PathFollower>(e)) {
            if (const Velocity *v = w.try_get<Velocity>(e); v != nullptr && is_finite(v->linear)) t.position += v->linear * dt;
        }
        const PrevTransform *prev = w.try_get<PrevTransform>(e);
        mp.last_delta = prev != nullptr && !w.has<JustSpawned>(e) ? t.position - prev->position : t.position - before;
        if (!is_finite(mp.last_delta)) mp.last_delta = {};
        const float moved = length(t.position - before);
        if (moved > max_move) max_move = moved;
    }
    pw.margin += max_move;
}

void run_carry_riders(World &w, PhysicsWorld &pw) {
    if (w.count<PlatformerMotor>() == 0) return;
    bool riders = false;
    for (auto [e, m] : w.view<PlatformerMotor>()) {
        (void)e;
        if (m.riding != Entity{}) {
            riders = true;
            break;
        }
    }
    bool rising = false;
    for (auto [e, mp] : w.view<MovingPlatform>()) {
        (void)e;
        if (mp.carry_riders && mp.last_delta.y < 0.0f) {
            rising = true;
            break;
        }
    }
    if (!riders && !rising) return;
    // Game code may have moved bodies since the last build.
    build_dynamic(w, pw);
    const TileWorld &tw = tiles_of(w);
    for (auto [e, m, t, c] : w.view<PlatformerMotor, Transform, Collider>()) {
        const Rect box = collider_box(c, t.position);
        if (!is_finite(box.min()) || !is_finite(box.size())) continue;
        const Vec2 start = t.position;
        Entity platform;
        float top = 0.0f;
        Vec2 delta;
        if (m.riding != Entity{}) {
            const MovingPlatform *mp = w.try_get<MovingPlatform>(m.riding);
            if (mp == nullptr || !w.alive(m.riding)) {
                m.riding = Entity{};
                continue;
            }
            delta = mp->last_delta;
            if (!mp->carry_riders || !is_finite(delta) || (delta.x == 0.0f && delta.y == 0.0f)) continue;
            platform = m.riding;
            gather_solids(w, pw, e, platformer_region(box, delta, 1.0f), m.solid_mask, true);
        } else if (rising) {
            // A rising platform that caught a body from above lifts it. Highest platform wins,
            // then lowest index.
            gather_solids(w, pw, e, box.expanded(1.0f), m.solid_mask, true);
            const float bottom = box.y + box.h;
            for (std::size_t i = 0; i < pw.solid_entities.size(); ++i) {
                const MovingPlatform *mp = w.try_get<MovingPlatform>(pw.solid_entities[i]);
                if (mp == nullptr || !mp->carry_riders || !(mp->last_delta.y < 0.0f)) continue;
                const Rect &p = gathered_rect(pw, i);
                if (!(bottom > p.y) || !(box.x < p.x + p.w && p.x < box.x + box.w)) continue;
                if (bottom > p.y - mp->last_delta.y + kOnTopTolerance) continue;   // came from below
                if (platform == Entity{} || p.y < top) {
                    platform = pw.solid_entities[i];
                    top = p.y;
                }
            }
            if (platform == Entity{}) continue;
            delta = Vec2{0.0f, top - bottom};
        } else {
            continue;
        }
        // The platform moved into the body, so it can't block this move.
        for (std::size_t i = 0; i < pw.solid_entities.size(); ++i) {
            if (pw.solid_entities[i] == platform) gathered_rect(pw, i) = Rect{};
        }
        // Move as if the body walked.
        MoveBoxOptions o;
        o.platformer = true;
        o.solids = pw.solid_rects;
        o.one_way = pw.one_way_rects;
        const BoxMove r = move_box(tw, box, delta, o);
        t.position += r.box.min() - box.min();
        const Collider *pc = w.try_get<Collider>(platform);
        const Transform *pt = w.try_get<Transform>(platform);
        if (pc != nullptr && pt != nullptr) {
            const Rect p = collider_box(*pc, pt->position);
            const Rect now = collider_box(c, t.position);
            const float sink = now.y + now.h - p.y;
            if (sink > 0.0f && sink <= kOnTopTolerance && now.x < p.x + p.w && p.x < now.x + now.w) {
                settle_on(t, c, p.y);
            }
        }
        // So later riders find this one where it is now.
        const float dist = length(t.position - start);
        if (dist > pw.margin) pw.margin = dist;
    }
}

void run_motors(World &w, PhysicsWorld &pw, float dt) {
    if (w.count<PlatformerMotor>() == 0 && w.count<TopDownMotor>() == 0) return;
    // Platforms and riders just moved.
    build_dynamic(w, pw);
    // Motors only move Transforms, so the solid layers stay valid for the whole loop.
    pw.solid_layers_fresh = true;
    const TileWorld &tw = tiles_of(w);
    for (auto [e, m, t, c, v] : w.view<PlatformerMotor, Transform, Collider, Velocity>()) {
        platformer_step(w, pw, tw, e, m, t, c, v, dt);
    }
    for (auto [e, m, t, c, v] : w.view<TopDownMotor, Transform, Collider, Velocity>()) {
        top_down_step(w, pw, tw, e, m, t, c, v, dt);
    }
    pw.solid_layers_fresh = false;
}

} // namespace detail

BoxMove move_body(World &w, Entity self, Vec2 delta, CollisionMask solid_mask, const MoveBoxOptions &o) {
    BoxMove r;
    Transform *t = w.try_get<Transform>(self);
    const Collider *c = w.try_get<Collider>(self);
    if (t == nullptr || c == nullptr) return r;
    const Rect box = collider_box(*c, t->position);
    MoveBoxOptions opts = o;
    detail::PhysicsWorld *pw = w.resources().try_get<detail::PhysicsWorld>();
    if (pw != nullptr) {
        detail::ensure_grids(w, *pw);
        const Vec2 d = is_finite(delta) ? delta : Vec2{};
        const float margin = 1.0f + std::max(o.snap_down, 0.0f) + std::max(o.corner_assist, 0.0f);
        detail::gather_solids(w, *pw, self,
                              o.platformer ? detail::platformer_region(box, d, margin) : detail::swept(box, d).expanded(margin),
                              solid_mask, o.platformer);
        opts.solids = pw->solid_rects;
        opts.one_way = pw->one_way_rects;
    }
    r = move_box(detail::tiles_of(w), box, delta, opts);
    const Vec2 moved = r.box.min() - box.min();
    t->position += moved;
    if (pw != nullptr) pw->margin += length(moved);
    return r;
}

}
