// Box movement against tiles and entity solids. X then Y, stopping flush on every tile line
// crossed, so one big step and many small ones end in the same place. Platformer mode stands the
// box on its bottom centre over slopes and steps up small ledges.
#include <lam/physics/tile_world.h>

#include <cmath>
#include <limits>

#include "detail/physics_world.h"
#include "detail/tile_math.h"

namespace lam {
namespace {

using detail::TileSpan;

constexpr float kInfinity = std::numeric_limits<float>::infinity();
// A support this close to the box bottom counts as touching.
constexpr float kGroundProbe = 1.0f / 64.0f;

bool finite(float v) { return std::isfinite(v); }
bool finite_rect(const Rect &r) { return finite(r.x) && finite(r.y) && finite(r.w) && finite(r.h); }

// The grid seen from one sweep direction: `along` is the axis being moved on.
struct Axis {
    const TileWorld *w = nullptr;
    bool horizontal = true;

    float size() const { return static_cast<float>(w->tile_size); }
    float origin() const { return horizontal ? w->origin.x : w->origin.y; }
    float origin_across() const { return horizontal ? w->origin.y : w->origin.x; }
    int count() const { return horizontal ? w->shapes.width() : w->shapes.height(); }
    int count_across() const { return horizontal ? w->shapes.height() : w->shapes.width(); }
    TileShape at(int along, int across) const {
        return horizontal ? detail::shape_at(*w, IVec2{along, across}) : detail::shape_at(*w, IVec2{across, along});
    }
    TileSpan span(float low, float high) const {
        return detail::tile_span(low, high, origin(), size(), count());
    }
    TileSpan span_across(float low, float high) const {
        return detail::tile_span(low, high, origin_across(), size(), count_across());
    }
    float edge(int line) const { return detail::tile_edge(line, origin(), size()); }
};

// The box's extent relative to its position on one axis: [position + low, position + high).
struct Extent {
    float low = 0.0f;
    float high = 0.0f;
};

struct Sweep {
    float position = 0.0f;
    bool hit = false;
};

// Stops at the first line where line_blocked() is true. The start must be clear.
template <class LineBlocked>
Sweep sweep_lines(const Axis &axis, Extent extent, float position, float delta, LineBlocked &&line_blocked) {
    if (delta == 0.0f) return Sweep{position, false};
    const float target = position + delta;
    const TileSpan now = axis.span(position + extent.low, position + extent.high);
    const TileSpan then = axis.span(target + extent.low, target + extent.high);
    if (delta > 0.0f) {
        // tile_span clamps lines to two past the grid, which bounds this loop.
        for (int line = now.last + 1; line <= then.last; ++line) {
            if (!line_blocked(line)) continue;
            float stopped = axis.edge(line) - extent.high;
            // Rounding can leave the box just over the line; step back until it isn't.
            while (axis.span(stopped + extent.low, stopped + extent.high).last >= line) {
                stopped = std::nextafter(stopped, -kInfinity);
            }
            // ...but never move a flush box backwards.
            if (stopped < position) stopped = position;
            return Sweep{stopped, true};
        }
    } else {
        for (int line = now.first - 1; line >= then.first; --line) {
            if (!line_blocked(line)) continue;
            float stopped = axis.edge(line + 1) - extent.low;
            while (axis.span(stopped + extent.low, stopped + extent.high).first <= line) {
                stopped = std::nextafter(stopped, kInfinity);
            }
            if (stopped > position) stopped = position;
            return Sweep{stopped, true};
        }
    }
    return Sweep{target, false};
}

// A full-block line test over `across` (top-down semantics).
struct FullLines {
    const Axis *axis;
    TileSpan across;
    bool operator()(int line) const {
        for (int i = across.first; i <= across.last; ++i) {
            if (detail::blocks_full(axis->at(line, i))) return true;
        }
        return false;
    }
};

Sweep sweep_full(const Axis &axis, Extent extent, float position, float delta, TileSpan across) {
    return sweep_lines(axis, extent, position, delta, FullLines{&axis, across});
}

// ---- Entity solids --------------------------------------------------------------------------

struct SolidSweep {
    float position = 0.0f;
    bool hit = false;
    std::int32_t index = -1;
};

// Rects the box already overlaps are ignored, so a body can always leave a crate pushed into it.
// Ties go to the lowest index.
SolidSweep sweep_solids(std::span<const Rect> solids, bool horizontal, const Rect &box, float delta) {
    const float position = horizontal ? box.x : box.y;
    const float size = horizontal ? box.w : box.h;
    SolidSweep out{position + delta, false, -1};
    if (delta == 0.0f) {
        out.position = position;
        return out;
    }
    for (std::size_t i = 0; i < solids.size(); ++i) {
        const Rect &s = solids[i];
        if (s.empty() || !finite_rect(s) || box.overlaps(s)) continue;
        if (horizontal) {
            if (!(s.y < box.y + box.h && box.y < s.y + s.h)) continue;
        } else {
            if (!(s.x < box.x + box.w && box.x < s.x + s.w)) continue;
        }
        const float s_lo = horizontal ? s.x : s.y;
        const float s_hi = horizontal ? s.x + s.w : s.y + s.h;
        if (delta > 0.0f) {
            if (s_lo < position + size) continue;                      // behind
            if (!(s_lo < out.position + size)) continue;                // not reached
            float stopped = s_lo - size;
            while (stopped + size > s_lo) stopped = std::nextafter(stopped, -kInfinity);
            if (stopped < position) stopped = position;
            if (stopped < out.position || !out.hit) {
                out = SolidSweep{stopped, true, static_cast<std::int32_t>(i)};
            }
        } else {
            if (s_hi > position) continue;
            if (!(s_hi > out.position)) continue;
            float stopped = s_hi;
            if (stopped > position) stopped = position;
            if (stopped > out.position || !out.hit) {
                out = SolidSweep{stopped, true, static_cast<std::int32_t>(i)};
            }
        }
    }
    return out;
}

bool overlaps_any(std::span<const Rect> solids, const Rect &box) {
    for (const Rect &s : solids) {
        if (!s.empty() && finite_rect(s) && box.overlaps(s)) return true;
    }
    return false;
}

// Combined result of a tile sweep and a solid sweep on one axis.
struct AxisMove {
    float position = 0.0f;
    bool hit = false;
    std::int32_t solid = -1;
};

AxisMove nearer(float delta, Sweep tiles, SolidSweep solids) {
    if (!solids.hit) return AxisMove{tiles.position, tiles.hit, -1};
    if (!tiles.hit) return AxisMove{solids.position, true, solids.index};
    const bool solid_first = delta > 0.0f ? solids.position <= tiles.position : solids.position >= tiles.position;
    if (solid_first) return AxisMove{solids.position, true, solids.index};
    return AxisMove{tiles.position, true, -1};
}

// ---- Top-down mode ----------------------------------------------------------------------------

bool tiles_blocked_full(const TileWorld &w, const Rect &box) {
    const float size = static_cast<float>(w.tile_size);
    const TileSpan cols = detail::tile_span(box.x, box.x + box.w, w.origin.x, size, w.shapes.width());
    const TileSpan rows = detail::tile_span(box.y, box.y + box.h, w.origin.y, size, w.shapes.height());
    for (int r = rows.first; r <= rows.last; ++r) {
        for (int c = cols.first; c <= cols.last; ++c) {
            if (detail::blocks_full(detail::shape_at(w, IVec2{c, r}))) return true;
        }
    }
    return false;
}

// Area of `box` inside blocking tiles. Exact within two tiles of the grid (the clamp).
float tile_overlap_area(const TileWorld &w, const Rect &box) {
    const float size = static_cast<float>(w.tile_size);
    const TileSpan cols = detail::tile_span(box.x, box.x + box.w, w.origin.x, size, w.shapes.width());
    const TileSpan rows = detail::tile_span(box.y, box.y + box.h, w.origin.y, size, w.shapes.height());
    float area = 0.0f;
    for (int r = rows.first; r <= rows.last; ++r) {
        for (int c = cols.first; c <= cols.last; ++c) {
            if (!detail::blocks_full(detail::shape_at(w, IVec2{c, r}))) continue;
            const Rect t = w.tile_rect(IVec2{c, r});
            const float ix = std::min(box.x + box.w, t.x + t.w) - std::max(box.x, t.x);
            const float iy = std::min(box.y + box.h, t.y + t.h) - std::max(box.y, t.y);
            if (ix > 0.0f && iy > 0.0f) area += ix * iy;
        }
    }
    return area;
}

struct TopDown {
    const TileWorld *w;
    std::span<const Rect> solids;

    // Sweep along `axis` for the box at (along = position, across = side).
    AxisMove sweep(const Axis &axis, const Rect &box, float delta) const {
        const bool h = axis.horizontal;
        const Extent extent{0.0f, h ? box.w : box.h};
        const float position = h ? box.x : box.y;
        const TileSpan across = h ? axis.span_across(box.y, box.y + box.h) : axis.span_across(box.x, box.x + box.w);
        const Sweep t = sweep_full(axis, extent, position, delta, across);
        const SolidSweep s = sweep_solids(solids, h, box, delta);
        return nearer(delta, t, s);
    }
};

struct Shift {
    bool possible = false;
    float amount = 0.0f;                                        // > 0
    float target = 0.0f;
};

// Returns the shifted side position, or `side` if nothing within `reach` clears the corner.
float assisted_side(const TopDown &td, const Axis &axis, const Rect &box, float delta, float reach,
                    std::int32_t blocking_solid) {
    const bool h = axis.horizontal;
    const Axis side_axis{axis.w, !h};
    const float side = h ? box.y : box.x;
    const float across_size = h ? box.h : box.w;
    const Extent across{0.0f, across_size};
    const TileSpan side_now = side_axis.span(side, side + across_size);

    // Only shifts that clear the outermost tile line on either side are worth trying.
    Shift candidates[4];
    int n = 0;
    {
        Shift neg;
        neg.target = side_axis.edge(side_now.last) - across.high;
        while (side_axis.span(neg.target + across.low, neg.target + across.high).last >= side_now.last) {
            neg.target = std::nextafter(neg.target, -kInfinity);
        }
        neg.amount = side - neg.target;
        Shift pos;
        pos.target = side_axis.edge(side_now.first + 1) - across.low;
        while (side_axis.span(pos.target + across.low, pos.target + across.high).first <= side_now.first) {
            pos.target = std::nextafter(pos.target, kInfinity);
        }
        pos.amount = pos.target - side;
        candidates[n++] = neg;
        candidates[n++] = pos;
    }
    // Round an entity solid: past its near edges.
    if (blocking_solid >= 0) {
        const Rect &s = td.solids[static_cast<std::size_t>(blocking_solid)];
        const float s_lo = h ? s.y : s.x;
        const float s_hi = h ? s.y + s.h : s.x + s.w;
        Shift neg;
        neg.target = s_lo - across_size;
        while (neg.target + across_size > s_lo) neg.target = std::nextafter(neg.target, -kInfinity);
        neg.amount = side - neg.target;
        Shift pos;
        pos.target = s_hi;
        pos.amount = pos.target - side;
        candidates[n++] = neg;
        candidates[n++] = pos;
    }

    for (int i = 0; i < n; ++i) {
        Shift &c = candidates[i];
        if (!(c.amount > 0.0f) || c.amount > reach) continue;
        // The way to there must be open...
        const AxisMove sideways = td.sweep(side_axis, box, c.target - side);
        if (sideways.hit) continue;
        // ...and the way ahead from there.
        const Rect there = h ? Rect{box.x, c.target, box.w, box.h} : Rect{c.target, box.y, box.w, box.h};
        const AxisMove ahead = td.sweep(axis, there, delta);
        const float start = h ? there.x : there.y;
        if (ahead.position == start) continue;
        c.possible = true;
    }
    // Smallest shift wins; ties go toward negative.
    const Shift *chosen = nullptr;
    bool chosen_negative = false;
    for (int i = 0; i < n; ++i) {
        const Shift &c = candidates[i];
        if (!c.possible) continue;
        const bool negative = c.target < side;
        if (chosen == nullptr || c.amount < chosen->amount ||
            (c.amount == chosen->amount && negative && !chosen_negative)) {
            chosen = &c;
            chosen_negative = negative;
        }
    }
    if (chosen == nullptr) return side;
    // Never slide further sideways than the forward distance lost, or corners would be a shortcut.
    const float budget = std::fabs(delta);
    if (budget >= chosen->amount) return chosen->target;
    const float step = chosen_negative ? -budget : budget;
    return td.sweep(side_axis, box, step).position;
}

BoxMove move_top_down(const TileWorld &w, Rect box, Vec2 delta, const MoveBoxOptions &o) {
    BoxMove r;
    r.box = box;
    if (tiles_blocked_full(w, box)) {
        // Started inside tiles: allow moves that don't deepen the overlap so it can walk out.
        const auto escape = [&](bool horizontal, float d) {
            if (d == 0.0f) return false;
            const SolidSweep s = sweep_solids(o.solids, horizontal, r.box, d);
            Rect moved = r.box;
            (horizontal ? moved.x : moved.y) = s.position;
            if (tile_overlap_area(w, moved) > tile_overlap_area(w, r.box)) return true;
            r.box = moved;
            if (s.hit && r.blocking_solid < 0) r.blocking_solid = s.index;
            return s.hit;
        };
        r.hit_x = escape(true, delta.x);
        r.hit_y = escape(false, delta.y);
        return r;
    }
    const TopDown td{&w, o.solids};
    const Axis axis_x{&w, true};
    const Axis axis_y{&w, false};
    const float reach = o.corner_assist > 0.0f ? o.corner_assist : 0.0f;   // NaN: off

    const AxisMove mx = td.sweep(axis_x, box, delta.x);
    r.box.x = mx.position;
    r.hit_x = mx.hit;
    if (mx.solid >= 0) r.blocking_solid = mx.solid;
    if (reach > 0.0f && delta.y == 0.0f && mx.hit && mx.position == box.x) {
        const float side = assisted_side(td, axis_x, box, delta.x, reach, mx.solid);
        if (side != box.y) {
            r.box = Rect{box.x, side, box.w, box.h};
            r.hit_x = false;
            r.blocking_solid = -1;
            return r;
        }
    }

    const AxisMove my = td.sweep(axis_y, r.box, delta.y);
    r.box.y = my.position;
    r.hit_y = my.hit;
    if (my.solid >= 0 && r.blocking_solid < 0) r.blocking_solid = my.solid;
    if (reach > 0.0f && delta.x == 0.0f && my.hit && my.position == box.y) {
        const float side = assisted_side(td, axis_y, r.box, delta.y, reach, my.solid);
        if (side != box.x) {
            r.box = Rect{side, box.y, box.w, box.h};
            r.hit_y = false;
            r.blocking_solid = -1;
            return r;
        }
    }
    r.grounded = my.hit && delta.y > 0.0f;
    if (r.grounded && my.solid >= 0) r.ground_solid = my.solid;
    return r;
}

// ---- Platformer mode ----------------------------------------------------------------------------

struct Support {
    bool found = false;
    float y = 0.0f;                                             // surface y (box bottom goes here)
    bool slope = false;
    bool tile = false;                                          // tile top, needs to sit exactly flush
    int row = 0;                                                // tile row of a tile top
    Vec2 normal{0.0f, -1.0f};
    std::int32_t solid = -1;                                    // index into solids, then one_way
};

struct Platformer {
    const TileWorld *w;
    const MoveBoxOptions *o;
    float size;
    float step;                                                 // bottom band the box may sink into

    TileShape at(int c, int r) const { return detail::shape_at(*w, IVec2{c, r}); }
    TileSpan cols(float lo, float hi) const {
        return detail::tile_span(lo, hi, w->origin.x, size, w->shapes.width());
    }
    TileSpan rows(float lo, float hi) const {
        return detail::tile_span(lo, hi, w->origin.y, size, w->shapes.height());
    }
    int col_of(float x) const { return detail::tile_index(x, w->origin.x, size, w->shapes.width()); }
    int row_of(float y) const { return detail::tile_index(y, w->origin.y, size, w->shapes.height()); }
    float row_top(int r) const { return detail::tile_edge(r, w->origin.y, size); }
    Rect tile_rect(int c, int r) const {
        return Rect{detail::tile_edge(c, w->origin.x, size), detail::tile_edge(r, w->origin.y, size), size, size};
    }
    float body_bottom(const Rect &box) const { return box.y + box.h - step; }

    // Solid tiles overlapping the box above its step band (slopes never count).
    bool body_blocked(const Rect &box) const {
        const TileSpan cs = cols(box.x, box.x + box.w);
        const TileSpan rs = rows(box.y, body_bottom(box));
        for (int r = rs.first; r <= rs.last; ++r) {
            for (int c = cs.first; c <= cs.last; ++c) {
                if (at(c, r) == TileShape::Solid) return true;
            }
        }
        return false;
    }

    // Highest slope surface at x within [lo, hi], ignoring slopes entirely above `head`.
    bool slope_at(float x, float lo, float hi, float head, float &y, TileShape &shape) const {
        const int c = col_of(x);
        const int r0 = row_of(lo) - 1;
        const int r1 = row_of(hi);
        bool found = false;
        for (int r = r0; r <= r1; ++r) {
            const TileShape s = at(c, r);
            if (!detail::is_slope(s)) continue;
            if (row_top(r) + size <= head) continue;
            const float sy = detail::slope_surface(s, tile_rect(c, r), x);
            if (sy < lo || sy > hi) continue;
            if (!found || sy < y) {
                y = sy;
                shape = s;
                found = true;
            }
        }
        return found;
    }

    // Highest support in [lo, hi]. One-ways only catch a box that started above them (b_start),
    // and nothing above `head` counts since the box would have to pass through it.
    Support find_support(const Rect &box, float lo, float hi, float b_start, const Rect &start,
                         bool allow_one_way, float head = -kInfinity) const {
        Support best;
        const auto offer = [&best](const Support &s) {
            if (!best.found || s.y < best.y) best = s;
        };
        const float cx = box.x + box.w * 0.5f;
        const int center_col = col_of(cx);
        float slope_y = 0.0f;
        TileShape slope_shape = TileShape::Empty;
        const bool has_slope = slope_at(cx, lo, hi + step, head, slope_y, slope_shape);

        // Entity tops first, so on a tie a solid or platform wins (riding needs to know it).
        const std::size_t n_solids = o->solids.size();
        for (std::size_t i = 0; i < n_solids + o->one_way.size(); ++i) {
            const bool one_way = i >= n_solids;
            if (one_way && (o->drop_through || !allow_one_way)) continue;
            const Rect &s = one_way ? o->one_way[i - n_solids] : o->solids[i];
            if (s.empty() || !finite_rect(s)) continue;
            if (!(s.x < box.x + box.w && box.x < s.x + s.w)) continue;
            if (start.overlaps(s)) continue;
            if (s.y < lo || s.y > hi || s.y < b_start - kGroundProbe) continue;
            if (s.y + s.h <= head) continue;
            Support sup;
            sup.found = true;
            sup.y = s.y;
            sup.solid = static_cast<std::int32_t>(i);
            offer(sup);
        }

        // Tile tops, nearest row first. Clamp rows in float so a huge move can't overflow int.
        if (std::isfinite(lo) && std::isfinite(hi)) {
            const int r0 = detail::limit_tile(std::ceil((lo - w->origin.y) / size), w->shapes.height());
            const int r1 = detail::limit_tile(std::floor((hi - w->origin.y) / size), w->shapes.height());
            const TileSpan cs = cols(box.x, box.x + box.w);
            for (int r = r0; r <= r1 && !(best.found && best.y < row_top(r)); ++r) {
                const float top = row_top(r);
                if (top < lo || top > hi || top + size <= head) continue;
                for (int c = cs.first; c <= cs.last; ++c) {
                    const TileShape s = at(c, r);
                    bool valid = false;
                    if (s == TileShape::Solid) {
                        // An exposed top, or the Solid under a ramp's low end in another column
                        // so a box hanging over the end rests on it instead of sinking in.
                        const TileShape above = at(c, r - 1);
                        valid = above == TileShape::Empty || above == TileShape::OneWay ||
                                (c != center_col && !has_slope && detail::is_slope(above));
                    } else if (s == TileShape::OneWay) {
                        valid = allow_one_way && !o->drop_through && top >= b_start - kGroundProbe;
                    }
                    if (!valid) continue;
                    // Foot on a slope just below this top: let the box sink in.
                    if (c != center_col && has_slope && slope_y > top && slope_y <= top + step) continue;
                    Support sup;
                    sup.found = true;
                    sup.y = top;
                    sup.tile = true;
                    sup.row = r;
                    offer(sup);
                    break;
                }
            }
        }
        if (has_slope && slope_y >= lo && slope_y <= hi) {
            Support sup;
            sup.found = true;
            sup.y = slope_y;
            sup.slope = true;
            sup.normal = detail::slope_normal(slope_shape);
            offer(sup);
        }
        return best;
    }

    // Places the box so its bottom is on the support (flush, never overlapping a tile top).
    float bottom_on(const Rect &box, const Support &s) const {
        float y = s.y - box.h;
        if (s.tile) {
            while (rows(y, y + box.h).last >= s.row) y = std::nextafter(y, -kInfinity);
        } else if (s.solid >= 0) {
            while (y + box.h > s.y) y = std::nextafter(y, -kInfinity);
        }
        return y;
    }

    // Walls for the X sweep: Solid tiles, and slope faces met edge-on, overlapping the body rows.
    AxisMove sweep_x(const Rect &box, float dx) const {
        const Axis axis{w, true};
        const float body_lo = box.y;
        const float body_hi = body_bottom(box);
        const TileSpan across = axis.span_across(body_lo, body_hi);
        const bool moving_right = dx > 0.0f;
        const auto line_blocked = [&](int col) {
            for (int r = across.first; r <= across.last; ++r) {
                const TileShape s = at(col, r);
                if (s == TileShape::Solid) return true;
                if (!detail::is_slope(s)) continue;
                const Rect t = tile_rect(col, r);
                const float face_top = detail::slope_edge_surface(s, t, moving_right);
                const float bottom = t.y + t.h;
                if (face_top < bottom && face_top < body_hi && body_lo < bottom) return true;
            }
            return false;
        };
        const Sweep t = sweep_lines(axis, Extent{0.0f, box.w}, box.x, dx, line_blocked);
        const SolidSweep s = sweep_solids(o->solids, true, box, dx);
        return nearer(dx, t, s);
    }

    // Ceilings for an upward move: every non-Empty, non-OneWay tile, and solids.
    AxisMove sweep_up(const Rect &box, float dy) const {
        const Axis axis{w, false};
        const TileSpan across = axis.span_across(box.x, box.x + box.w);
        const Sweep t = sweep_full(axis, Extent{0.0f, box.h}, box.y, dy, across);
        const SolidSweep s = sweep_solids(o->solids, false, box, dy);
        return nearer(dy, t, s);
    }
};

BoxMove move_platformer(const TileWorld &w, Rect box, Vec2 delta, const MoveBoxOptions &o) {
    BoxMove r;
    r.box = box;
    const float half_w = box.w * 0.5f;
    const float three_q_h = box.h * 0.75f;
    const Platformer p{&w, &o, static_cast<float>(w.tile_size), half_w < three_q_h ? half_w : three_q_h};
    if (p.body_blocked(box)) {
        r.hit_x = true;
        r.hit_y = true;
        return r;
    }
    const float b0 = box.y + box.h;
    const bool grounded0 = p.find_support(box, b0 - kGroundProbe, b0 + kGroundProbe, b0, box, true).found;

    // X, then fit the foot to the floor it walked onto (up a slope or onto a ledge corner).
    Rect cur = box;
    if (delta.x != 0.0f) {
        const AxisMove mx = p.sweep_x(cur, delta.x);
        cur.x = mx.position;
        r.hit_x = mx.hit;
        if (mx.solid >= 0) r.blocking_solid = mx.solid;
        const float moved = std::fabs(cur.x - box.x);
        if (moved > 0.0f) {
            const float b = cur.y + cur.h;
            const Support lift = p.find_support(cur, b - (p.step + moved), b, b, box, false, cur.y);
            if (lift.found && lift.y < b) {
                const float y = p.bottom_on(cur, lift);
                const AxisMove up = p.sweep_up(cur, y - cur.y);
                if (!up.hit) {
                    cur.y = y;
                } else {
                    cur.x = box.x;                                      // no head room: a wall
                    r.hit_x = true;
                }
            }
        }
    }

    // Y: ceilings going up; the first support going down.
    bool landed = false;
    Support ground;
    if (delta.y < 0.0f) {
        const AxisMove my = p.sweep_up(cur, delta.y);
        cur.y = my.position;
        r.hit_y = my.hit;
        if (my.solid >= 0 && r.blocking_solid < 0) r.blocking_solid = my.solid;
    } else if (delta.y > 0.0f) {
        const float b = cur.y + cur.h;
        const Support s = p.find_support(cur, b, b + delta.y, b, box, true);
        if (s.found) {
            cur.y = p.bottom_on(cur, s);
            landed = true;
            ground = s;
            r.hit_y = true;
        } else {
            cur.y += delta.y;
        }
    }

    // Ground snap: a box that was standing and is not jumping keeps its feet down slopes.
    if (!landed && grounded0 && o.snap_down > 0.0f && delta.y >= 0.0f) {
        const float b = cur.y + cur.h;
        const Support s = p.find_support(cur, b, b + o.snap_down, b, box, true);
        if (s.found) cur.y = p.bottom_on(cur, s);
    }

    const float b = cur.y + cur.h;
    ground = p.find_support(cur, b - kGroundProbe, b + kGroundProbe, b, cur, true);
    r.box = cur;
    r.grounded = ground.found;
    if (ground.found) {
        r.on_slope = ground.slope;
        r.ground_normal = ground.normal;
        r.ground_solid = ground.solid;
    }
    return r;
}

} // namespace

BoxMove move_box(const TileWorld &w, Rect box, Vec2 delta, const MoveBoxOptions &o) {
    if (!finite_rect(box) || !finite(delta.x) || !finite(delta.y)) {
        BoxMove r;
        r.box = box;
        return r;
    }
    if (o.platformer) return move_platformer(w, box, delta, o);
    return move_top_down(w, box, delta, o);
}

bool box_blocked(const TileWorld &w, Rect box, std::span<const Rect> solids) {
    if (!finite_rect(box)) return true;
    return tiles_blocked_full(w, box) || overlaps_any(solids, box);
}

std::optional<Vec2> nearest_free_spot(const TileWorld &w, Rect box, int max_tiles) {
    if (!finite_rect(box)) return std::nullopt;
    if (!tiles_blocked_full(w, box)) return box.min();
    // Half-tile raster: fine enough to centre a small box in a one-tile gap, coarse enough to
    // stay cheap. Scan order (top-down, left-right) breaks ties.
    const float step = static_cast<float>(w.tile_size) * 0.5f;
    const Vec2 c = box.center();
    const Vec2 half = box.size() * 0.5f;
    const float home_x = w.origin.x + std::floor((c.x - w.origin.x) / step) * step;
    const float home_y = w.origin.y + std::floor((c.y - w.origin.y) / step) * step;
    const int reach = (max_tiles > 0 ? max_tiles : 0) * 2;
    float best = kInfinity;
    std::optional<Vec2> spot;
    for (int sy = -reach; sy <= reach; ++sy) {
        for (int sx = -reach; sx <= reach; ++sx) {
            const Vec2 at{home_x + static_cast<float>(sx) * step, home_y + static_cast<float>(sy) * step};
            const float d = distance_sq(at, c);
            if (!(d < best)) continue;
            const Rect candidate = Rect::from_center(at, half);
            if (tiles_blocked_full(w, candidate)) continue;
            best = d;
            spot = candidate.min();
        }
    }
    return spot;
}

}

// ---- Rigid bodies and tile flags (private, detail/physics_world.h) -------------------------------

namespace lam::detail {

bool rigid_blocked(const TileWorld &w, const Rect &box) {
    if (!has_tiles(w)) return false;
    if (!finite_rect(box)) return true;
    const float size = static_cast<float>(w.tile_size);
    const TileSpan cols = tile_span(box.x, box.x + box.w, w.origin.x, size, w.shapes.width());
    const TileSpan rows = tile_span(box.y, box.y + box.h, w.origin.y, size, w.shapes.height());
    for (int r = rows.first; r <= rows.last; ++r) {
        for (int c = cols.first; c <= cols.last; ++c) {
            if (shape_at(w, IVec2{c, r}) == TileShape::Solid) return true;
        }
    }
    return false;
}

RigidMove move_rigid(const TileWorld &w, Rect box, Vec2 delta) {
    RigidMove out;
    out.box = box;
    if (!finite_rect(box) || !finite(delta.x) || !finite(delta.y)) return out;
    if (!has_tiles(w)) {
        out.box = box.translated(delta);
        return out;
    }
    if (rigid_blocked(w, box)) {
        out.hit_x = delta.x != 0.0f;
        out.hit_y = delta.y != 0.0f;
        out.started_blocked = true;
        return out;
    }
    const Axis ax{&w, true};
    const Axis ay{&w, false};
    {
        const TileSpan across = ax.span_across(box.y, box.y + box.h);
        const auto blocked = [&](int col) {
            for (int r = across.first; r <= across.last; ++r) {
                if (ax.at(col, r) == TileShape::Solid) return true;
            }
            return false;
        };
        const Sweep s = sweep_lines(ax, Extent{0.0f, box.w}, box.x, delta.x, blocked);
        out.box.x = s.position;
        out.hit_x = s.hit;
    }
    {
        const TileSpan across = ay.span_across(out.box.x, out.box.x + out.box.w);
        const bool down = delta.y > 0.0f;
        // Going down, any one-way row we meet was below the box, so it holds.
        const auto blocked = [&](int row) {
            for (int c = across.first; c <= across.last; ++c) {
                const TileShape s = ay.at(row, c);
                if (s == TileShape::Solid || (down && s == TileShape::OneWay)) return true;
            }
            return false;
        };
        const Sweep s = sweep_lines(ay, Extent{0.0f, box.h}, box.y, delta.y, blocked);
        out.box.y = s.position;
        out.hit_y = s.hit;
    }
    return out;
}

bool find_flag(const TileWorld &w, const Rect &box, std::uint8_t flag, IVec2 &cell) {
    if (!has_tiles(w) || !finite_rect(box)) return false;
    const float size = static_cast<float>(w.tile_size);
    const TileSpan cols = tile_span(box.x, box.x + box.w, w.origin.x, size, w.shapes.width());
    const TileSpan rows = tile_span(box.y, box.y + box.h, w.origin.y, size, w.shapes.height());
    for (int r = rows.first; r <= rows.last; ++r) {
        for (int c = cols.first; c <= cols.last; ++c) {
            if ((flags_at(w, IVec2{c, r}) & flag) != 0) {
                cell = IVec2{c, r};
                return true;
            }
        }
    }
    return false;
}

}
