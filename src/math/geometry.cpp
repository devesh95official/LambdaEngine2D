#include <lam/math/geometry.h>

#include <cmath>

namespace lam {
namespace {

Vec2 closest_point(const Rect &r, Vec2 p) {
    return {clamp(p.x, r.x, r.x + r.w), clamp(p.y, r.y, r.y + r.h)};
}

bool strictly_inside(const Rect &r, Vec2 p) {
    return p.x > r.x && p.x < r.x + r.w && p.y > r.y && p.y < r.y + r.h;
}

// One slab of the segment/box test. Returns false when the segment misses the slab.
bool slab(float origin, float dir, float lo, float hi, float &t_min, float &t_max, float &entry_normal,
          bool &entered) {
    if (std::fabs(dir) < 1e-12f) return origin >= lo && origin <= hi;
    float t1 = (lo - origin) / dir;
    float t2 = (hi - origin) / dir;
    float n = -1.0f;
    if (t1 > t2) {
        const float tmp = t1;
        t1 = t2;
        t2 = tmp;
        n = 1.0f;
    }
    if (t1 > t_min) {
        t_min = t1;
        entry_normal = n;
        entered = true;
    }
    if (t2 < t_max) t_max = t2;
    return t_min <= t_max;
}

} // namespace

bool overlaps(const Circle &a, const Circle &b) {
    const float r = a.radius + b.radius;
    return distance_sq(a.center, b.center) < r * r;
}

bool overlaps(const Circle &c, const Rect &r) {
    return distance_sq(closest_point(r, c.center), c.center) < c.radius * c.radius;
}

bool contains(const Circle &c, Vec2 p) { return distance_sq(c.center, p) <= c.radius * c.radius; }

bool point_in_cone(Vec2 origin, Vec2 dir_unit, float range, float half_angle, Vec2 p) {
    const Vec2 d = p - origin;
    const float dist_sq = length_sq(d);
    if (dist_sq == 0.0f) return true;
    if (dist_sq > range * range) return false;
    const float cos_limit = det_cos(half_angle);
    return dot(d, dir_unit) >= cos_limit * std::sqrt(dist_sq);
}

std::optional<SegmentHit> intersect(const Segment &s, const Rect &r) {
    if (strictly_inside(r, s.a)) return SegmentHit{0.0f, s.a, {}};
    const Vec2 d = s.b - s.a;
    float t_min = 0.0f, t_max = 1.0f, nx = 0.0f, ny = 0.0f;
    bool ex = false, ey = false;
    if (!slab(s.a.x, d.x, r.x, r.x + r.w, t_min, t_max, nx, ex)) return std::nullopt;
    const float t_after_x = t_min;
    if (!slab(s.a.y, d.y, r.y, r.y + r.h, t_min, t_max, ny, ey)) return std::nullopt;
    Vec2 normal{};
    if (ey && t_min > t_after_x) normal = {0.0f, ny};
    else if (ex) normal = {nx, 0.0f};
    else if (ey) normal = {0.0f, ny};
    return SegmentHit{t_min, s.a + d * t_min, normal};
}

std::optional<SegmentHit> intersect(const Segment &s, const Circle &c) {
    const Vec2 d = s.b - s.a;
    const Vec2 f = s.a - c.center;
    const float cc = length_sq(f) - c.radius * c.radius;
    if (cc < 0.0f) return SegmentHit{0.0f, s.a, {}};
    const float a = length_sq(d);
    if (a == 0.0f) return std::nullopt;
    const float b = 2.0f * dot(f, d);
    const float disc = b * b - 4.0f * a * cc;
    if (disc < 0.0f) return std::nullopt;
    const float t = (-b - std::sqrt(disc)) / (2.0f * a);
    if (t < 0.0f || t > 1.0f) return std::nullopt;
    const Vec2 p = s.a + d * t;
    return SegmentHit{t, p, normalize_or(p - c.center)};
}

std::optional<SegmentHit> sweep_circle_rect(Vec2 from, Vec2 to, float radius, const Rect &r) {
    if (distance_sq(closest_point(r, from), from) < radius * radius) return SegmentHit{0.0f, from, {}};
    const Segment seg{from, to};
    const auto hit = intersect(seg, r.expanded(radius));
    if (!hit) return std::nullopt;
    const Vec2 p = hit->point;
    const bool left = p.x < r.x, right = p.x > r.x + r.w;
    const bool top = p.y < r.y, bottom = p.y > r.y + r.h;
    if ((left || right) && (top || bottom)) {
        const Vec2 corner{left ? r.x : r.x + r.w, top ? r.y : r.y + r.h};
        return intersect(seg, Circle{corner, radius});
    }
    return hit;
}

} // namespace lam
