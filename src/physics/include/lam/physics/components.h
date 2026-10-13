#pragma once
// Physics components. A Collider without RigidBody is Kinematic if it has Velocity or a mover
// component, otherwise Static. Static bodies never move, so place them exactly where they rest.
// Give every mover a Velocity: contacts use it as a kinematic body's speed.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <lam/core/assert.h>
#include <lam/core/id.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/rect.h>
#include <lam/math/vec2.h>
namespace lam {
using CollisionMask = std::uint32_t;
struct Velocity { Vec2 linear; };                               // px/s
enum class ShapeType : std::uint8_t { Box, Circle, Polygon };
struct Collider {
    ShapeType shape = ShapeType::Box;
    Vec2 offset;                                                // centre relative to Transform.position
    Vec2 half{8, 8};                                            // Box half extents (also the motor body)
    float radius = 8;                                           // Circle
    std::uint16_t polygon = 0;                                  // PolygonShapes id
    CollisionMask layer = 1;                                    // layers this collider is on
    CollisionMask mask = 0xFFFFFFFFu;                           // layers it reports contacts with
    bool trigger = false;                                       // reports overlaps, never solid
    bool one_way = false;                                       // blocks only from above
};
// a and b interact when each one's layer is in the other's mask. Every pair except Static-Static
// reports events, even motors that don't block each other. Blocking is separate: see solid_mask.
enum class BodyType : std::uint8_t { Static, Kinematic, Dynamic };
struct RigidBody {                                              // linear only
    BodyType type = BodyType::Dynamic;
    float mass = 1;
    float restitution = 0;
    float friction = 0.2f;
    float gravity_scale = 1;
    float linear_damping = 0;
    bool sleeping = false;
};
// Swept circle moved by its Velocity (needs Transform, no Collider). Each hit spends a pierce;
// when it stops it rests at the contact with zero Velocity. The game destroys it, not physics.
struct Projectile {
    float radius = 1;
    CollisionMask mask = 0xFFFFFFFFu;
    Entity owner;                                               // never hit
    std::uint16_t pierce = 0;                                   // extra hits before it stops
    bool stop_at_tiles = true;
};
struct Interactable { StrId kind; float radius = 12; };
inline Rect collider_box(const Collider &c, Vec2 position) {
    const Vec2 half = c.shape == ShapeType::Box ? c.half : Vec2{c.radius, c.radius};
    return Rect::from_center(position + c.offset, half);
}
struct PolygonShapes {                                          // World resource; convex, CCW, 3..8 verts
    static constexpr std::size_t kMaxVerts = 8;
    std::vector<std::array<Vec2, kMaxVerts>> verts;             // id - 1
    std::vector<std::uint8_t> counts;
    std::uint16_t add(std::span<const Vec2> v) {
        LAM_CHECK(v.size() >= 3 && v.size() <= kMaxVerts, "polygon needs 3..8 vertices");
        LAM_CHECK(verts.size() < 65535u, "too many polygons");
        std::array<Vec2, kMaxVerts> a{};
        for (std::size_t i = 0; i < v.size(); ++i) a[i] = v[i];
        verts.push_back(a);
        counts.push_back(static_cast<std::uint8_t>(v.size()));
        return static_cast<std::uint16_t>(verts.size());
    }
    std::span<const Vec2> get(std::uint16_t id) const {
        if (id == 0 || id > verts.size()) return {};
        return std::span<const Vec2>(verts[id - 1u].data(), counts[id - 1u]);
    }
};
}
LAM_REFLECT_ENUM(lam::ShapeType, "box", "circle", "polygon")
LAM_REFLECT_ENUM(lam::BodyType, "static", "kinematic", "dynamic")
LAM_REFLECT(lam::Velocity, linear)
LAM_REFLECT(lam::Collider, shape, offset, half, radius, polygon, layer, mask, trigger, one_way)
LAM_REFLECT(lam::RigidBody, type, mass, restitution, friction, gravity_scale, linear_damping, sleeping)
LAM_REFLECT(lam::Projectile, radius, mask, owner, pierce, stop_at_tiles)
LAM_REFLECT(lam::Interactable, kind, radius)
