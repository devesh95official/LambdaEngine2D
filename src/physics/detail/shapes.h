#pragma once
// Narrowphase shapes: box, circle and convex polygon (SAT), plus swept circle/ray tests.
// No rotation; Transform.rotation is ignored.
#include <array>
#include <cstdint>
#include <optional>
#include <lam/math/geometry.h>
#include <lam/math/rect.h>
#include <lam/math/vec2.h>
#include <lam/physics/components.h>
namespace lam::detail {

// A collider in world space. Polygon views point into PolygonShapes and die with it.
struct ShapeView {
    ShapeType type = ShapeType::Box;
    Vec2 center;                                                // world pixels
    Vec2 half;                                                  // Box half extents; others: AABB half
    float radius = 0;                                           // Circle
    const Vec2 *local = nullptr;                                // Polygon vertices (local), else null
    std::uint8_t count = 0;                                     // Polygon vertex count
    Rect aabb;                                                  // bounding box
};

// An unknown polygon id falls back to a Box of `half`.
ShapeView make_shape(const Collider &c, Vec2 position, const PolygonShapes *polygons);

// normal points from the first shape to the second; point is approximate.
struct ContactInfo {
    Vec2 normal{0, -1};
    float depth = 0;
    Vec2 point;
};
// True when depth + skin > 0. Order doesn't matter. The reported depth ignores the skin, so it
// can be <= 0 when skin > 0.
bool collide(const ShapeView &a, const ShapeView &b, ContactInfo &out, float skin = 0);

// radius 0 is a ray. Starting inside gives t = 0 and a zero normal.
std::optional<SegmentHit> sweep_shape(const ShapeView &s, Vec2 from, Vec2 to, float radius);

// World-space convex polygon, 3..8 vertices.
std::optional<SegmentHit> sweep_polygon(const Vec2 *verts, int count, Vec2 from, Vec2 to, float radius);

// Exact Minkowski sum with rounded corners.
std::optional<SegmentHit> sweep_rect(const Rect &r, Vec2 from, Vec2 to, float radius);

bool point_near_shape(const ShapeView &s, Vec2 p, float radius);

}
