#pragma once
// Shape tests used by physics, steering and gameplay.
#include <optional>
#include <lam/math/rect.h>
namespace lam {
struct Circle { Vec2 center; float radius = 0; };
struct Segment { Vec2 a, b; };
// Touching is not overlap.
bool overlaps(const Circle &a, const Circle &b);
bool overlaps(const Circle &c, const Rect &r);
// Points on the edge are inside.
bool contains(const Circle &c, Vec2 p);
// p within `range` of origin and within half_angle (radians) of dir_unit. p == origin -> true.
bool point_in_cone(Vec2 origin, Vec2 dir_unit, float range, float half_angle, Vec2 p);
// First contact along a segment: t in [0, 1] of a->b, the point and the surface normal there.
// A segment starting inside reports t = 0 and a zero normal.
struct SegmentHit { float t = 0; Vec2 point; Vec2 normal; };
std::optional<SegmentHit> intersect(const Segment &s, const Rect &r);
std::optional<SegmentHit> intersect(const Segment &s, const Circle &c);
// A circle of `radius` moving from -> to against a rectangle: first contact of its centre path.
std::optional<SegmentHit> sweep_circle_rect(Vec2 from, Vec2 to, float radius, const Rect &r);
}
