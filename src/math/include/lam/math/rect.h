#pragma once
// Axis-aligned rectangles: top-left plus size, half-open [x, x+w).
#include <lam/math/vec2.h>
namespace lam {
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    static constexpr Rect from_center(Vec2 c, Vec2 half) {
        return {c.x - half.x, c.y - half.y, half.x * 2, half.y * 2};
    }
    static constexpr Rect from_min_max(Vec2 mn, Vec2 mx) { return {mn.x, mn.y, mx.x - mn.x, mx.y - mn.y}; }
    constexpr Vec2 min() const { return {x, y}; }
    constexpr Vec2 max() const { return {x + w, y + h}; }
    constexpr Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    constexpr Vec2 size() const { return {w, h}; }
    // w <= 0 || h <= 0 (NaN sizes count as empty).
    constexpr bool empty() const { return !(w > 0) || !(h > 0); }
    constexpr bool contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    // Touching edges do not overlap.
    constexpr bool overlaps(const Rect &o) const {
        return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h;
    }
    constexpr Rect expanded(float by) const { return {x - by, y - by, w + by * 2, h + by * 2}; }
    constexpr Rect translated(Vec2 d) const { return {x + d.x, y + d.y, w, h}; }
    constexpr bool operator==(const Rect &) const = default;
};
struct IRect {
    int x = 0, y = 0, w = 0, h = 0;
    constexpr bool empty() const { return w <= 0 || h <= 0; }
    constexpr bool contains(IVec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    constexpr bool overlaps(const IRect &o) const {
        return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h;
    }
    constexpr bool operator==(const IRect &) const = default;
};
// The overlap of a and b; no overlap -> w or h == 0.
constexpr Rect intersection(const Rect &a, const Rect &b) {
    const float x0 = a.x > b.x ? a.x : b.x;
    const float y0 = a.y > b.y ? a.y : b.y;
    const float x1 = (a.x + a.w) < (b.x + b.w) ? a.x + a.w : b.x + b.w;
    const float y1 = (a.y + a.h) < (b.y + b.h) ? a.y + a.h : b.y + b.h;
    return {x0, y0, x1 > x0 ? x1 - x0 : 0.0f, y1 > y0 ? y1 - y0 : 0.0f};
}
constexpr IRect intersection(const IRect &a, const IRect &b) {
    const int x0 = a.x > b.x ? a.x : b.x;
    const int y0 = a.y > b.y ? a.y : b.y;
    const int x1 = (a.x + a.w) < (b.x + b.w) ? a.x + a.w : b.x + b.w;
    const int y1 = (a.y + a.h) < (b.y + b.h) ? a.y + a.h : b.y + b.h;
    return {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
}
constexpr Rect to_rect(const IRect &r) {
    return {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w), static_cast<float>(r.h)};
}
}
