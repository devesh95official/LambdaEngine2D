// Amanatides-Woo rays and swept circles through the tile grid. Cells are clamped to two tiles
// past the grid, so every walk is bounded.
#include <lam/physics/tile_world.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "detail/shapes.h"
#include "detail/tile_math.h"

namespace lam {
namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();

bool finite2(Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

Rect grid_rect(const TileWorld &w) {
    const float s = static_cast<float>(w.tile_size);
    return Rect{w.origin.x, w.origin.y, static_cast<float>(w.shapes.width()) * s,
                static_cast<float>(w.shapes.height()) * s};
}

// Liang-Barsky, inclusive edges. Infinite bounds mean that side isn't clipped.
bool clip_to(Vec2 lo_v, Vec2 hi_v, Vec2 a, Vec2 d, float &t0, float &t1) {
    const float lo[2] = {lo_v.x, lo_v.y};
    const float hi[2] = {hi_v.x, hi_v.y};
    const float o[2] = {a.x, a.y};
    const float dir[2] = {d.x, d.y};
    for (int k = 0; k < 2; ++k) {
        if (dir[k] == 0.0f) {
            if (o[k] < lo[k] || o[k] > hi[k]) return false;
            continue;
        }
        float ta = (lo[k] - o[k]) / dir[k];
        float tb = (hi[k] - o[k]) / dir[k];
        if (ta > tb) {
            const float tmp = ta;
            ta = tb;
            tb = tmp;
        }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return false;
    }
    return true;
}

// Grid rect grown by `margin` on open sides and unbounded on closed ones.
void open_bounds(const TileWorld &w, float margin, Vec2 &lo, Vec2 &hi) {
    const Rect g = grid_rect(w);
    const std::uint8_t open = detail::open_sides(w);
    lo = Vec2{(open & kSideLeft) != 0 ? g.x - margin : -kInfinity, (open & kSideTop) != 0 ? g.y - margin : -kInfinity};
    hi = Vec2{(open & kSideRight) != 0 ? g.x + g.w + margin : kInfinity,
              (open & kSideBottom) != 0 ? g.y + g.h + margin : kInfinity};
}

// Strictly past a closed side and not past an open one.
bool beyond_closed(const TileWorld &w, Vec2 p) {
    const Rect g = grid_rect(w);
    const auto beyond = static_cast<std::uint8_t>((p.x < g.x ? kSideLeft : 0) | (p.x > g.x + g.w ? kSideRight : 0) |
                                                  (p.y < g.y ? kSideTop : 0) | (p.y > g.y + g.h ? kSideBottom : 0));
    return beyond != 0 && (beyond & detail::open_sides(w)) == 0;
}

// Includes both end cells. A ray through a diagonal corner between two blockers is blocked.
// Cells are recomputed each step rather than accumulated so exact corners stay exact.
template <class Blocks>
std::optional<IVec2> first_cell(const TileWorld &w, Vec2 a, Vec2 b, Blocks &&blocks) {
    if (!detail::has_tiles(w)) return std::nullopt;
    const float size = static_cast<float>(w.tile_size);
    const int width = w.shapes.width();
    const int height = w.shapes.height();
    if (detail::open_sides(w) != 0) {
        // Cells past an open side are empty, so clip the segment there.
        float t0 = 0.0f;
        float t1 = 1.0f;
        Vec2 lo, hi;
        open_bounds(w, 0.0f, lo, hi);
        if (!clip_to(lo, hi, a, b - a, t0, t1)) return std::nullopt;
        const Vec2 d = b - a;
        const Vec2 ca = a + d * t0;
        const Vec2 cb = a + d * t1;
        a = ca;
        b = cb;
    }
    int tx = detail::tile_index(a.x, w.origin.x, size, width);
    int ty = detail::tile_index(a.y, w.origin.y, size, height);
    const int end_x = detail::tile_index(b.x, w.origin.x, size, width);
    const int end_y = detail::tile_index(b.y, w.origin.y, size, height);
    if (blocks(IVec2{tx, ty})) return IVec2{tx, ty};
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const int step_x = dx > 0.0f ? 1 : -1;
    const int step_y = dy > 0.0f ? 1 : -1;
    int left_x = end_x > tx ? end_x - tx : tx - end_x;
    int left_y = end_y > ty ? end_y - ty : ty - end_y;
    // t (0 at a, 1 at b) at which the segment reaches the next boundary on each axis.
    const auto reach_x = [&]() {
        return (detail::tile_edge(step_x > 0 ? tx + 1 : tx, w.origin.x, size) - a.x) / dx;
    };
    const auto reach_y = [&]() {
        return (detail::tile_edge(step_y > 0 ? ty + 1 : ty, w.origin.y, size) - a.y) / dy;
    };
    while (left_x > 0 || left_y > 0) {
        bool move_x = left_x > 0;
        bool move_y = left_y > 0;
        if (move_x && move_y) {
            const float t_x = reach_x();
            const float t_y = reach_y();
            move_x = t_x <= t_y;
            move_y = t_y <= t_x;
            // Exact corner: allowed to brush one neighbour, but not to slip between two diagonal ones.
            if (move_x && move_y && blocks(IVec2{tx + step_x, ty}) && blocks(IVec2{tx, ty + step_y})) {
                return IVec2{tx + step_x, ty};
            }
        }
        if (move_x) {
            tx += step_x;
            --left_x;
        }
        if (move_y) {
            ty += step_y;
            --left_y;
        }
        if (blocks(IVec2{tx, ty})) return IVec2{tx, ty};
    }
    return std::nullopt;
}

} // namespace

bool tile_ray_clear(const TileWorld &w, Vec2 a, Vec2 b) {
    if (!finite2(a) || !finite2(b)) return false;
    return !first_cell(w, a, b, [&w](IVec2 c) { return detail::blocks_full(detail::shape_at(w, c)); }).has_value();
}

std::optional<IVec2> tile_raycast(const TileWorld &w, Vec2 a, Vec2 b, bool opaque_only) {
    if (!finite2(a) || !finite2(b)) {
        const float size = static_cast<float>(w.tile_size);
        return IVec2{detail::tile_index(a.x, w.origin.x, size, w.shapes.width()),
                     detail::tile_index(a.y, w.origin.y, size, w.shapes.height())};
    }
    if (opaque_only) return first_cell(w, a, b, [&w](IVec2 c) { return (detail::flags_at(w, c) & kTileOpaque) != 0; });
    return first_cell(w, a, b, [&w](IVec2 c) { return detail::blocks_full(detail::shape_at(w, c)); });
}

namespace detail {

std::optional<TileSweepHit> sweep_circle_tiles(const TileWorld &w, Vec2 from, Vec2 to, float radius,
                                               bool skip_start_inside) {
    if (!finite2(from) || !finite2(to) || !has_tiles(w)) return std::nullopt;
    const float r = radius > 0.0f ? radius : 0.0f;
    const float size = static_cast<float>(w.tile_size);
    const int width = w.shapes.width();
    const int height = w.shapes.height();
    const Vec2 d = to - from;
    float t_start = 0.0f;
    float t_end = 1.0f;
    if (open_sides(w) != 0) {
        // Cells past an open side are empty; clip to the grid plus the radius and a tile.
        Vec2 lo, hi;
        open_bounds(w, r + size, lo, hi);
        if (!clip_to(lo, hi, from, d, t_start, t_end)) return std::nullopt;
    }
    if (closed_sides(w) != 0) {
        const Vec2 p = from + d * t_start;
        if (beyond_closed(w, p)) {
            if (skip_start_inside) return std::nullopt;
            return TileSweepHit{t_start, p, {}, IVec2{tile_index(p.x, w.origin.x, size, width),
                                                     tile_index(p.y, w.origin.y, size, height)}};
        }
    }
    // Any tile the circle touches is within `band` cells of its centre's cell. Clamp the band in
    // float to the grid size so huge radii can't overflow.
    const float band_cells = std::ceil(r / size);
    const float band_limit = static_cast<float>(width + height + 4);
    const bool whole_grid = !(band_cells < band_limit);
    const int band = 1 + (whole_grid ? width + height + 4 : static_cast<int>(band_cells));
    const Vec2 p0 = from + d * t_start;
    int cx = tile_index(p0.x, w.origin.x, size, width);
    int cy = tile_index(p0.y, w.origin.y, size, height);
    const int sx = d.x > 0.0f ? 1 : (d.x < 0.0f ? -1 : 0);
    const int sy = d.y > 0.0f ? 1 : (d.y < 0.0f ? -1 : 0);
    float t_max_x = sx != 0 ? (tile_edge(sx > 0 ? cx + 1 : cx, w.origin.x, size) - from.x) / d.x : kInfinity;
    float t_max_y = sy != 0 ? (tile_edge(sy > 0 ? cy + 1 : cy, w.origin.y, size) - from.y) / d.y : kInfinity;
    const float t_delta_x = sx != 0 ? size / std::fabs(d.x) : kInfinity;
    const float t_delta_y = sy != 0 ? size / std::fabs(d.y) : kInfinity;

    std::optional<TileSweepHit> best;
    float t_entry = t_start;
    const auto visit = [&](int ccx, int ccy) {
        for (int oy = -band; oy <= band; ++oy) {
            for (int ox = -band; ox <= band; ++ox) {
                const IVec2 cell{ccx + ox, ccy + oy};
                const TileShape s = shape_at(w, cell);
                if (!blocks_full(s)) continue;
                const Rect tr = w.tile_rect(cell);
                std::optional<SegmentHit> h;
                if (s == TileShape::Solid) {
                    h = sweep_rect(tr, from, to, r);
                } else {
                    Vec2 verts[4];
                    const int n = tile_polygon(s, tr, verts);
                    h = sweep_polygon(verts, n, from, to, r);
                }
                if (!h) continue;
                if (skip_start_inside && h->t == 0.0f && h->normal.x == 0.0f && h->normal.y == 0.0f) continue;
                const bool better = !best || h->t < best->t ||
                                    (h->t == best->t && (cell.y < best->cell.y ||
                                                         (cell.y == best->cell.y && cell.x < best->cell.x)));
                if (better) best = TileSweepHit{h->t, h->point, h->normal, cell};
            }
        }
    };
    const int max_steps = (width + height + 8) * 2 + 8;
    for (int i = 0; i < max_steps; ++i) {
        visit(cx, cy);
        if (whole_grid || (best && best->t <= t_entry)) break;
        const float next = t_max_x < t_max_y ? t_max_x : t_max_y;
        if (!(next <= t_end)) break;
        if (best && best->t < next) break;
        if (t_max_x < t_max_y) {
            cx += sx;
            t_entry = t_max_x;
            t_max_x += t_delta_x;
        } else if (t_max_y < t_max_x) {
            cy += sy;
            t_entry = t_max_y;
            t_max_y += t_delta_y;
        } else {
            cx += sx;
            cy += sy;
            t_entry = t_max_x;
            t_max_x += t_delta_x;
            t_max_y += t_delta_y;
        }
        if (cx < -2 - band || cy < -2 - band || cx > width + 1 + band || cy > height + 1 + band) break;
    }
    return best;
}

} // namespace detail
}
