// Narrowphase tests. Ties are broken by fixed rules, never memory order, so results are
// deterministic.
#include "detail/shapes.h"

#include <cmath>
#include <limits>

namespace lam::detail {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

bool finite2(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

// A shape as a convex polygon in world space (boxes become 4 vertices).
struct Poly {
    std::array<Vec2, 8> v{};
    int n = 0;
    Vec2 centroid;
};

Poly as_poly(const ShapeView &s, float grow) {
    Poly p;
    if (s.type == ShapeType::Polygon && s.local != nullptr && s.count >= 3) {
        p.n = s.count;
        for (int i = 0; i < p.n; ++i) p.v[static_cast<std::size_t>(i)] = s.center + s.local[i];
    } else {
        const Vec2 h{s.half.x + grow, s.half.y + grow};
        p.n = 4;
        p.v[0] = {s.center.x - h.x, s.center.y - h.y};
        p.v[1] = {s.center.x + h.x, s.center.y - h.y};
        p.v[2] = {s.center.x + h.x, s.center.y + h.y};
        p.v[3] = {s.center.x - h.x, s.center.y + h.y};
    }
    Vec2 sum;
    for (int i = 0; i < p.n; ++i) sum += p.v[static_cast<std::size_t>(i)];
    p.centroid = sum / static_cast<float>(p.n);
    return p;
}

// Outward unit normal of edge i (works for either winding).
Vec2 edge_normal(const Vec2 *v, int n, Vec2 centroid, int i) {
    const Vec2 a = v[i];
    const Vec2 b = v[(i + 1) % n];
    Vec2 nrm = normalize_or(Vec2{b.y - a.y, a.x - b.x}, Vec2{});
    if (dot(nrm, (a + b) * 0.5f - centroid) < 0.0f) nrm = -nrm;
    return nrm;
}

void project(const Poly &p, Vec2 axis, float &lo, float &hi) {
    lo = kInf;
    hi = -kInf;
    for (int i = 0; i < p.n; ++i) {
        const float d = dot(p.v[static_cast<std::size_t>(i)], axis);
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
}

bool box_box(const ShapeView &a, const ShapeView &b, ContactInfo &out, float skin) {
    const float dx = b.center.x - a.center.x;
    const float dy = b.center.y - a.center.y;
    const float px = a.half.x + skin + b.half.x - std::fabs(dx);
    if (!(px > 0.0f)) return false;
    const float py = a.half.y + skin + b.half.y - std::fabs(dy);
    if (!(py > 0.0f)) return false;
    if (px < py) {
        out.normal = {dx < 0.0f ? -1.0f : 1.0f, 0.0f};
        out.depth = px - skin;
    } else {
        out.normal = {0.0f, dy < 0.0f ? -1.0f : 1.0f};
        out.depth = py - skin;
    }
    const float x0 = std::fmax(a.center.x - a.half.x, b.center.x - b.half.x);
    const float x1 = std::fmin(a.center.x + a.half.x, b.center.x + b.half.x);
    const float y0 = std::fmax(a.center.y - a.half.y, b.center.y - b.half.y);
    const float y1 = std::fmin(a.center.y + a.half.y, b.center.y + b.half.y);
    out.point = {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f};
    return true;
}

bool circle_circle(const ShapeView &a, const ShapeView &b, ContactInfo &out, float skin) {
    const Vec2 d = b.center - a.center;
    const float r = a.radius + skin + b.radius;
    const float d2 = length_sq(d);
    if (!(d2 < r * r)) return false;
    const float dist = std::sqrt(d2);
    out.normal = dist > 1e-6f ? d / dist : Vec2{1.0f, 0.0f};
    out.depth = r - dist - skin;
    out.point = a.center + out.normal * a.radius;
    return true;
}

// Box `a` against circle `b`; normal from a to b.
bool box_circle(const ShapeView &a, const ShapeView &b, ContactInfo &out, float skin) {
    const Vec2 h{a.half.x + skin, a.half.y + skin};
    const Vec2 rel = b.center - a.center;
    const Vec2 clamped{clamp(rel.x, -h.x, h.x), clamp(rel.y, -h.y, h.y)};
    if (!finite2(rel)) return false;
    if (rel.x != clamped.x || rel.y != clamped.y) {
        const Vec2 d = rel - clamped;
        const float d2 = length_sq(d);
        if (!(d2 < b.radius * b.radius)) return false;
        const float dist = std::sqrt(d2);
        out.normal = d / dist;
        out.depth = b.radius - dist - skin;
        out.point = a.center + clamped;
        return true;
    }
    const float ex = h.x - std::fabs(rel.x);
    const float ey = h.y - std::fabs(rel.y);
    if (ex < ey) {
        out.normal = {rel.x < 0.0f ? -1.0f : 1.0f, 0.0f};
        out.depth = ex + b.radius - skin;
    } else {
        out.normal = {0.0f, rel.y < 0.0f ? -1.0f : 1.0f};
        out.depth = ey + b.radius - skin;
    }
    out.point = b.center;
    return true;
}

// SAT. Polygons can't be grown like boxes, so the skin is a separation allowance per axis.
bool poly_poly(const Poly &a, const Poly &b, ContactInfo &out, float skin) {
    float best = kInf;
    Vec2 axis{0.0f, -1.0f};
    for (int side = 0; side < 2; ++side) {
        const Poly &p = side == 0 ? a : b;
        for (int i = 0; i < p.n; ++i) {
            const Vec2 n = edge_normal(p.v.data(), p.n, p.centroid, i);
            if (n.x == 0.0f && n.y == 0.0f) continue;
            float a0, a1, b0, b1;
            project(a, n, a0, a1);
            project(b, n, b0, b1);
            const float overlap = std::fmin(a1, b1) - std::fmax(a0, b0);
            if (!(overlap > -skin)) return false;
            if (overlap < best) {
                best = overlap;
                axis = n;
            }
        }
    }
    if (dot(axis, b.centroid - a.centroid) < 0.0f) axis = -axis;
    out.normal = axis;
    out.depth = best;
    // Deepest vertex of b along -axis.
    float lo = kInf;
    for (int i = 0; i < b.n; ++i) {
        const float d = dot(b.v[static_cast<std::size_t>(i)], axis);
        if (d < lo) {
            lo = d;
            out.point = b.v[static_cast<std::size_t>(i)];
        }
    }
    return true;
}

// Circle `c` (radius r) against polygon p; normal from the circle to the polygon.
bool circle_poly(Vec2 c, float r, const Poly &p, ContactInfo &out) {
    float best = kInf;
    Vec2 axis{0.0f, -1.0f};
    const auto test = [&](Vec2 n) {
        float p0, p1;
        project(p, n, p0, p1);
        const float cc = dot(c, n);
        const float overlap = std::fmin(p1, cc + r) - std::fmax(p0, cc - r);
        if (!(overlap > 0.0f)) return false;
        if (overlap < best) {
            best = overlap;
            axis = n;
        }
        return true;
    };
    for (int i = 0; i < p.n; ++i) {
        const Vec2 n = edge_normal(p.v.data(), p.n, p.centroid, i);
        if (n.x == 0.0f && n.y == 0.0f) continue;
        if (!test(n)) return false;
    }
    float nearest = kInf;
    Vec2 vertex;
    for (int i = 0; i < p.n; ++i) {
        const float d2 = distance_sq(p.v[static_cast<std::size_t>(i)], c);
        if (d2 < nearest) {
            nearest = d2;
            vertex = p.v[static_cast<std::size_t>(i)];
        }
    }
    const Vec2 vn = normalize_or(c - vertex, Vec2{});
    if ((vn.x != 0.0f || vn.y != 0.0f) && !test(vn)) return false;
    if (dot(axis, p.centroid - c) < 0.0f) axis = -axis;
    out.normal = axis;
    out.depth = best;
    out.point = c + axis * r;
    return true;
}

// Ray a + d t (t in [0, 1]) against a rect; strict (grazing an edge or a corner misses).
// `inside` is set when the ray starts strictly inside.
bool ray_rect(Vec2 a, Vec2 d, const Rect &r, float &t_hit, Vec2 &normal, bool &inside) {
    float t0 = 0.0f;
    float t1 = 1.0f;
    Vec2 n{};
    bool entered = false;
    const float lo[2] = {r.x, r.y};
    const float hi[2] = {r.x + r.w, r.y + r.h};
    const float o[2] = {a.x, a.y};
    const float dir[2] = {d.x, d.y};
    for (int k = 0; k < 2; ++k) {
        if (dir[k] == 0.0f) {
            if (!(o[k] > lo[k] && o[k] < hi[k])) return false;
            continue;
        }
        float ta = (lo[k] - o[k]) / dir[k];
        float tb = (hi[k] - o[k]) / dir[k];
        float sign = -1.0f;
        if (ta > tb) {
            const float tmp = ta;
            ta = tb;
            tb = tmp;
            sign = 1.0f;
        }
        if (ta > t0) {
            t0 = ta;
            n = k == 0 ? Vec2{sign, 0.0f} : Vec2{0.0f, sign};
            entered = true;
        }
        if (tb < t1) t1 = tb;
    }
    if (!(t0 < t1)) return false;
    inside = !entered;
    t_hit = t0;
    normal = n;
    return true;
}

// Ray a + d t against a circle; strict (tangent misses). Start inside: t = 0.
bool ray_circle(Vec2 a, Vec2 d, Vec2 c, float r, float &t_hit, Vec2 &normal, bool &inside) {
    const Vec2 f = a - c;
    const float cc = length_sq(f) - r * r;
    if (cc < 0.0f) {
        inside = true;
        t_hit = 0.0f;
        normal = {};
        return true;
    }
    const float qa = length_sq(d);
    if (qa == 0.0f) return false;
    const float qb = 2.0f * dot(f, d);
    const float disc = qb * qb - 4.0f * qa * cc;
    if (!(disc > 0.0f)) return false;
    const float t = (-qb - std::sqrt(disc)) / (2.0f * qa);
    if (t < 0.0f || t > 1.0f) return false;
    inside = false;
    t_hit = t;
    normal = normalize_or(a + d * t - c, Vec2{});
    return true;
}

float point_rect_distance_sq(Vec2 p, const Rect &r) {
    const Vec2 q{clamp(p.x, r.x, r.x + r.w), clamp(p.y, r.y, r.y + r.h)};
    return distance_sq(p, q);
}

// Signed distance from p to a convex polygon (negative inside).
float polygon_distance(const Vec2 *v, int n, Vec2 p) {
    Vec2 centroid;
    for (int i = 0; i < n; ++i) centroid += v[i];
    centroid = centroid / static_cast<float>(n);
    float max_plane = -kInf;
    for (int i = 0; i < n; ++i) {
        const Vec2 nrm = edge_normal(v, n, centroid, i);
        const float d = dot(p - v[i], nrm);
        if (d > max_plane) max_plane = d;
    }
    if (max_plane <= 0.0f) return max_plane;
    float best = kInf;
    for (int i = 0; i < n; ++i) {
        const Vec2 a = v[i];
        const Vec2 b = v[(i + 1) % n];
        const Vec2 e = b - a;
        const float len2 = length_sq(e);
        const float s = len2 > 0.0f ? clamp(dot(p - a, e) / len2, 0.0f, 1.0f) : 0.0f;
        const float d2 = distance_sq(p, a + e * s);
        if (d2 < best) best = d2;
    }
    return std::sqrt(best);
}

} // namespace

ShapeView make_shape(const Collider &c, Vec2 position, const PolygonShapes *polygons) {
    ShapeView v;
    v.type = c.shape;
    v.center = position + c.offset;
    if (!finite2(v.center)) {
        v.aabb = Rect{};
        return v;
    }
    if (c.shape == ShapeType::Circle) {
        v.radius = c.radius > 0.0f ? c.radius : 0.0f;
        v.half = {v.radius, v.radius};
    } else if (c.shape == ShapeType::Polygon && polygons != nullptr) {
        const std::span<const Vec2> verts = polygons->get(c.polygon);
        if (verts.size() >= 3) {
            v.local = verts.data();
            v.count = static_cast<std::uint8_t>(verts.size());
            Vec2 lo{kInf, kInf};
            Vec2 hi{-kInf, -kInf};
            for (const Vec2 p : verts) {
                lo = min(lo, p);
                hi = max(hi, p);
            }
            v.aabb = Rect::from_min_max(v.center + lo, v.center + hi);
            v.half = v.aabb.size() * 0.5f;
            return v;
        }
        v.type = ShapeType::Box;
        v.half = {std::fabs(c.half.x), std::fabs(c.half.y)};
    } else {
        v.type = ShapeType::Box;
        v.half = {std::fabs(c.half.x), std::fabs(c.half.y)};
    }
    v.aabb = Rect::from_center(v.center, v.half);
    return v;
}

bool collide(const ShapeView &a, const ShapeView &b, ContactInfo &out, float skin) {
    const bool pa = a.type == ShapeType::Polygon && a.local != nullptr;
    const bool pb = b.type == ShapeType::Polygon && b.local != nullptr;
    const bool ca = a.type == ShapeType::Circle;
    const bool cb = b.type == ShapeType::Circle;
    if (!pa && !pb) {
        if (!ca && !cb) return box_box(a, b, out, skin);
        if (ca && cb) return circle_circle(a, b, out, skin);
        if (!ca && cb) return box_circle(a, b, out, skin);
        // circle a, box b: test b against a (skin belongs to a, so grow its radius instead).
        ShapeView grown = a;
        grown.radius += skin;
        if (!box_circle(b, grown, out, 0.0f)) return false;
        out.normal = -out.normal;
        out.depth -= skin;
        return true;
    }
    if (ca) {
        if (!circle_poly(a.center, a.radius + skin, as_poly(b, 0.0f), out)) return false;
        out.depth -= skin;
        return true;
    }
    if (cb) {
        // Grow the circle by the skin, same as above, so argument order doesn't matter.
        if (!circle_poly(b.center, b.radius + skin, as_poly(a, 0.0f), out)) return false;
        out.normal = -out.normal;
        out.depth -= skin;
        return true;
    }
    return poly_poly(as_poly(a, 0.0f), as_poly(b, 0.0f), out, skin);
}

std::optional<SegmentHit> sweep_rect(const Rect &r, Vec2 from, Vec2 to, float radius) {
    if (!finite2(from) || !finite2(to)) return std::nullopt;
    const Vec2 d = to - from;
    if (radius <= 0.0f) {
        float t = 0.0f;
        Vec2 n;
        bool inside = false;
        if (!ray_rect(from, d, r, t, n, inside)) return std::nullopt;
        if (inside) return SegmentHit{0.0f, from, {}};
        return SegmentHit{t, from + d * t, n};
    }
    if (point_rect_distance_sq(from, r) < radius * radius) return SegmentHit{0.0f, from, {}};
    float best = kInf;
    Vec2 best_n;
    const auto try_rect = [&](const Rect &piece) {
        float t = 0.0f;
        Vec2 n;
        bool inside = false;
        if (ray_rect(from, d, piece, t, n, inside) && !inside && t < best) {
            best = t;
            best_n = n;
        }
    };
    try_rect(Rect{r.x - radius, r.y, r.w + radius * 2.0f, r.h});
    try_rect(Rect{r.x, r.y - radius, r.w, r.h + radius * 2.0f});
    const Vec2 corners[4] = {{r.x, r.y}, {r.x + r.w, r.y}, {r.x, r.y + r.h}, {r.x + r.w, r.y + r.h}};
    for (const Vec2 corner : corners) {
        float t = 0.0f;
        Vec2 n;
        bool inside = false;
        if (ray_circle(from, d, corner, radius, t, n, inside) && !inside && t < best) {
            best = t;
            best_n = n;
        }
    }
    if (!(best <= 1.0f)) return std::nullopt;
    return SegmentHit{best, from + d * best, best_n};
}

std::optional<SegmentHit> sweep_polygon(const Vec2 *verts, int count, Vec2 from, Vec2 to, float radius) {
    if (count < 3 || !finite2(from) || !finite2(to)) return std::nullopt;
    const float r = radius > 0.0f ? radius : 0.0f;
    const float start = polygon_distance(verts, count, from);
    if (start < r || (r == 0.0f && start < 0.0f)) return SegmentHit{0.0f, from, {}};
    Vec2 centroid;
    for (int i = 0; i < count; ++i) centroid += verts[i];
    centroid = centroid / static_cast<float>(count);
    const Vec2 d = to - from;
    float best = kInf;
    Vec2 best_n;
    for (int i = 0; i < count; ++i) {
        const Vec2 n = edge_normal(verts, count, centroid, i);
        const float denom = dot(d, n);
        if (!(denom < 0.0f)) continue;
        const Vec2 p0 = verts[i] + n * r;
        const Vec2 p1 = verts[(i + 1) % count] + n * r;
        const float t = dot(p0 - from, n) / denom;
        if (t < 0.0f || t > 1.0f || !(t < best)) continue;
        const Vec2 q = from + d * t;
        const Vec2 e = p1 - p0;
        const float len2 = length_sq(e);
        const float s = len2 > 0.0f ? dot(q - p0, e) / len2 : 0.0f;
        if (s < 0.0f || s > 1.0f) continue;
        best = t;
        best_n = n;
    }
    if (r > 0.0f) {
        for (int i = 0; i < count; ++i) {
            float t = 0.0f;
            Vec2 n;
            bool inside = false;
            if (ray_circle(from, d, verts[i], r, t, n, inside) && !inside && t < best) {
                best = t;
                best_n = n;
            }
        }
    }
    if (!(best <= 1.0f)) return std::nullopt;
    return SegmentHit{best, from + d * best, best_n};
}

std::optional<SegmentHit> sweep_shape(const ShapeView &s, Vec2 from, Vec2 to, float radius) {
    if (s.type == ShapeType::Circle) {
        const float r = s.radius + (radius > 0.0f ? radius : 0.0f);
        float t = 0.0f;
        Vec2 n;
        bool inside = false;
        if (!finite2(from) || !finite2(to)) return std::nullopt;
        if (!ray_circle(from, to - from, s.center, r, t, n, inside)) return std::nullopt;
        if (inside) return SegmentHit{0.0f, from, {}};
        return SegmentHit{t, from + (to - from) * t, n};
    }
    if (s.type == ShapeType::Polygon && s.local != nullptr) {
        std::array<Vec2, 8> w{};
        for (int i = 0; i < s.count; ++i) w[static_cast<std::size_t>(i)] = s.center + s.local[i];
        return sweep_polygon(w.data(), s.count, from, to, radius);
    }
    return sweep_rect(Rect::from_center(s.center, s.half), from, to, radius);
}

bool point_near_shape(const ShapeView &s, Vec2 p, float radius) {
    const float r = radius > 0.0f ? radius : 0.0f;
    if (s.type == ShapeType::Circle) return distance(p, s.center) <= s.radius + r;
    if (s.type == ShapeType::Polygon && s.local != nullptr) {
        std::array<Vec2, 8> w{};
        for (int i = 0; i < s.count; ++i) w[static_cast<std::size_t>(i)] = s.center + s.local[i];
        return polygon_distance(w.data(), s.count, p) <= r;
    }
    return point_rect_distance_sq(p, Rect::from_center(s.center, s.half)) <= r * r;
}

}
