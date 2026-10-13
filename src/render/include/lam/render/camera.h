#pragma once
// Camera components (presentation only, never hashed) and the per-world CameraView.
#include <cstdint>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/rect.h>
#include <lam/math/transform.h>
#include <lam/math/vec2.h>
namespace lam {
struct Camera2D {                                               // highest priority wins per world
    Vec2 position;                                              // centre, world pixels
    float zoom = 1;                                             // pixel-perfect: snapped to n or 1/n
    float rotation = 0;
    bool pixel_snap = true;                                     // backend rounds view-space vertices
    int priority = 0;
};
// Goes on the camera. Aims at the target's interpolated position + offset (the feet by default).
// A target with no Transform leaves the camera where it is.
struct CameraFollow {
    Entity target;
    float half_life = 0.08f;
    float lookahead_seconds = 0;
    float lookahead_max = 0;
    float lookahead_half_life = 0.25f;
    Rect dead_zone;                                             // relative to the centre; empty = none
    float snap_distance = 128;                                  // jumps further than this snap
    bool snap_next = false;                                     // cut instead of gliding, once
    Vec2 offset;                                                // e.g. {0, -16} to aim above the feet
};
// Goes on the camera. Keeps the view inside `world`, centring on any axis where it doesn't fit.
struct CameraBounds { Rect world; bool enabled = false; };
struct CameraShake {
    float trauma = 0;
    float decay_per_second = 2.4f;
    float max_offset = 5;
    float frequency_hz = 26;
    float exponent = 2;
    float time = 0;
};
struct CameraView {                                             // World resource
    Vec2 center;
    float zoom = 1;
    float rotation = 0;
    IVec2 virtual_size{480, 270};
    Mat2x3 world_to_screen;                                     // world px -> virtual px
    Mat2x3 screen_to_world;
    Rect visible_world;                                         // culling rect
    std::uint8_t view_id = 0;                                   // RenderQueue view this frame
    bool pixel_snap = true;
    bool valid = false;                                         // false until the first camera_view
};
inline Vec2 world_to_screen(const CameraView &v, Vec2 p) { return v.world_to_screen.apply(p); }
inline Vec2 screen_to_world(const CameraView &v, Vec2 p) { return v.screen_to_world.apply(p); }
// shake_offset in world pixels. pixel_perfect snaps zoom to n or 1/n.
CameraView make_camera_view(const Camera2D &c, IVec2 virtual_size, Vec2 shake_offset, bool pixel_perfect);
}
LAM_REFLECT_FLAGS(lam::Camera2D, lam::kSerialize | lam::kSnapshot, position, zoom, rotation, pixel_snap, priority)
LAM_REFLECT_FLAGS(lam::CameraFollow, lam::kSerialize | lam::kSnapshot, target, half_life, lookahead_seconds,
                  lookahead_max, lookahead_half_life, dead_zone, snap_distance, snap_next, offset)
LAM_REFLECT_FLAGS(lam::CameraBounds, lam::kSerialize | lam::kSnapshot, world, enabled)
LAM_REFLECT_FLAGS(lam::CameraShake, lam::kSerialize | lam::kSnapshot, trauma, decay_per_second, max_offset,
                  frequency_hz, exponent, time)
