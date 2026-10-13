// Steering helpers. Fixed rotation tables keep results identical on every platform.
#include <lam/physics/steering.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "detail/tile_math.h"

namespace lam {
namespace {

constexpr float kNoDistance = 0.001f;
// Back off inside this fraction of keep_distance. The band stops it flip-flopping every tick.
constexpr float kRingInner = 0.75f;
// A flight direction is good when the box can slide this far along it.
constexpr float kFleeProbe = 24.0f;
// cos 45, written out so every platform rotates by exactly the same amount.
constexpr float kDiagonal = 0.70710678f;
// Slack for the division in box_path_clear, so an exact corner-to-corner pass errs on blocked.
constexpr float kSweepSlack = 1.0f / 1024.0f;

bool finite2(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

Vec2 unit_or(Vec2 v, Vec2 fallback) {
    const float size = std::sqrt(v.x * v.x + v.y * v.y);
    if (!(size >= kNoDistance)) return fallback;
    return Vec2{v.x / size, v.y / size};
}

} // namespace

Vec2 sight_point(const Rect &body) { return Vec2{body.x + body.w * 0.5f, body.y + body.h - 1.0f}; }

bool can_see(const TileWorld &w, Vec2 eye, Vec2 target, float range) {
    // Range check first; most pairs fail it.
    if (!finite2(eye) || !finite2(target) || !(distance(eye, target) <= range)) return false;
    return !tile_raycast(w, eye, target, true).has_value();
}

Vec2 seek(Vec2 from, Vec2 to, float speed) {
    const Vec2 delta = to - from;
    const float size = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    if (!(size >= kNoDistance)) return Vec2{};
    return delta * (speed / size);
}

Vec2 hold_ring(Vec2 pos, Vec2 target, float keep_distance, float approach_speed, float orbit_speed, int orbit_sign) {
    if (!(keep_distance > 0.0f)) return seek(pos, target, approach_speed);
    const float d = distance(pos, target);
    if (d > keep_distance) return seek(pos, target, approach_speed);
    // Exactly on the target there's no "away", so step off sideways.
    const float sign = orbit_sign < 0 ? -1.0f : 1.0f;
    const Vec2 away = unit_or(pos - target, Vec2{sign, 0.0f});
    if (d < keep_distance * kRingInner) return away * approach_speed;
    // +y is down the screen, so (-y, x) of the outward direction is the clockwise tangent.
    return Vec2{-away.y * orbit_speed * sign, away.x * orbit_speed * sign};
}

bool box_path_clear(const TileWorld &w, Rect rel_box, Vec2 a, Vec2 b) {
    if (!finite2(a) || !finite2(b) || !finite2(rel_box.min()) || !finite2(rel_box.size())) return false;
    const Rect box = rel_box.translated(a);
    const Rect end = rel_box.translated(b);
    if (!finite2(box.min()) || !finite2(end.min())) return false;
    const float s = static_cast<float>(w.tile_size);
    const float dx = end.x - box.x;
    const float dy = end.y - box.y;
    const float top = std::min(box.y, end.y);
    const float bottom = std::max(box.y, end.y) + box.h;
    // Past a closed side everything is solid, so a sweep that reaches there is blocked.
    // Touching the edge from inside doesn't count.
    if (const std::uint8_t closed = detail::closed_sides(w); closed != 0) {
        const std::uint8_t open = detail::open_sides(w);
        const float left = std::min(box.x, end.x);
        const float right = std::max(box.x, end.x) + box.w;
        const float grid_left = w.origin.x;
        const float grid_top = w.origin.y;
        const float grid_right = detail::tile_edge(w.shapes.width(), w.origin.x, s);
        const float grid_bottom = detail::tile_edge(w.shapes.height(), w.origin.y, s);
        // The extent reaches rows (columns) that are not beyond an open side.
        const bool rows_closed = ((open & kSideTop) == 0 || bottom > grid_top) &&
                                 ((open & kSideBottom) == 0 || top < grid_bottom);
        const bool cols_closed = ((open & kSideLeft) == 0 || right > grid_left) &&
                                 ((open & kSideRight) == 0 || left < grid_right);
        if ((rows_closed && (((closed & kSideLeft) != 0 && left < grid_left) ||
                             ((closed & kSideRight) != 0 && right > grid_right))) ||
            (cols_closed && (((closed & kSideTop) != 0 && top < grid_top) ||
                             ((closed & kSideBottom) != 0 && bottom > grid_bottom)))) {
            return false;
        }
    }
    const detail::TileSpan rows = detail::tile_span(top, bottom, w.origin.y, s, w.shapes.height());
    for (int r = rows.first; r <= rows.last; ++r) {
        // The box's top edge y overlaps row r while row_top - h < y < row_top + s.
        const float row_top = detail::tile_edge(r, w.origin.y, s);
        const float y_low = row_top - box.h;
        const float y_high = row_top + s;
        float t0 = 0.0f;
        float t1 = 1.0f;
        if (dy == 0.0f) {
            if (!(box.y > y_low && box.y < y_high)) continue;
        } else {
            const float enter = ((dy > 0.0f ? y_low : y_high) - box.y) / dy;
            const float leave = ((dy > 0.0f ? y_high : y_low) - box.y) / dy;
            t0 = std::max(t0, enter);
            t1 = std::min(t1, leave);
            if (!(t0 < t1)) continue;
        }
        // No slack on the end points, or a box flush against a wall would count as blocked.
        const float x0 = t0 <= 0.0f ? box.x : box.x + dx * t0;
        const float x1 = t1 >= 1.0f ? end.x : box.x + dx * t1;
        const float slack0 = (t0 <= 0.0f || dx == 0.0f) ? 0.0f : kSweepSlack;
        const float slack1 = (t1 >= 1.0f || dx == 0.0f) ? 0.0f : kSweepSlack;
        const float low = std::min(x0 - slack0, x1 - slack1);
        const float high = std::max(x0 + slack0, x1 + slack1) + box.w;
        const detail::TileSpan cols = detail::tile_span(low, high, w.origin.x, s, w.shapes.width());
        for (int c = cols.first; c <= cols.last; ++c) {
            if (detail::blocks_full(detail::shape_at(w, IVec2{c, r}))) return false;
        }
    }
    return true;
}

Vec2 flee(const TileWorld &w, Rect box, Vec2 pos, Vec2 threat, float speed) {
    const Vec2 away = unit_or(pos - threat, Vec2{1.0f, 0.0f});
    // Rotations of `away` as (cos, sin): 0, +45, -45, +90, -90, +135, -135 degrees.
    static constexpr std::array<Vec2, 7> kTurns{{
        {1.0f, 0.0f},
        {kDiagonal, kDiagonal},
        {kDiagonal, -kDiagonal},
        {0.0f, 1.0f},
        {0.0f, -1.0f},
        {-kDiagonal, kDiagonal},
        {-kDiagonal, -kDiagonal},
    }};
    for (const Vec2 &turn : kTurns) {
        const Vec2 dir{away.x * turn.x - away.y * turn.y, away.x * turn.y + away.y * turn.x};
        if (box_path_clear(w, box, pos, pos + dir * kFleeProbe)) return dir * speed;
    }
    return away * speed;
}

bool pick_wander_goal(const TileWorld &w, Rect box, Vec2 from, Vec2 home, float radius, Rng &rng, Vec2 &out,
                      int attempts) {
    if (!finite2(from) || !finite2(home) || !(radius >= 0.0f) || !std::isfinite(radius)) return false;
    for (int i = 0; i < attempts; ++i) {
        const float angle = rng.unit() * kTau;
        // sqrt for an even spread over the disc.
        const float reach = radius * std::sqrt(rng.unit());
        const Vec2 candidate = home + from_angle(angle, reach);
        if (box_path_clear(w, box, from, candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

bool pick_wander_goal(const TileWorld &w, Rect box, Vec2 home, float radius, Rng &rng, Vec2 &out, int attempts) {
    return pick_wander_goal(w, box, home, home, radius, rng, out, attempts);
}

}
