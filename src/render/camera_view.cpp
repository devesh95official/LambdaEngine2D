// make_camera_view: screen = half + R(rotation) * zoom * (world - centre), with the translation
// rounded when pixel_snap is on. Rotation is clockwise on screen, as in raylib.
#include <cmath>

#include <lam/math/scalar.h>
#include <lam/render/camera.h>

namespace lam {
namespace {

// Bad zoom -> 1. Pixel-perfect snaps to n or 1/n.
float sanitize_zoom(float zoom, bool pixel_perfect) {
    if (!is_finite(zoom) || !(zoom > 0.0f)) return 1.0f;
    if (!pixel_perfect) return zoom;
    if (zoom >= 1.0f) return std::floor(zoom + 0.5f);
    const float n = std::floor(1.0f / zoom + 0.5f);
    return n >= 1.0f ? 1.0f / n : 1.0f;
}

} // namespace

CameraView make_camera_view(const Camera2D &c, IVec2 virtual_size, Vec2 shake_offset,
                            bool pixel_perfect) {
    CameraView v;
    v.virtual_size = virtual_size;
    v.zoom = sanitize_zoom(c.zoom, pixel_perfect);
    v.rotation = is_finite(c.rotation) ? c.rotation : 0.0f;
    v.pixel_snap = c.pixel_snap;

    Vec2 centre = c.position + (is_finite(shake_offset) ? shake_offset : Vec2{});
    if (!is_finite(centre)) centre = {};
    const Vec2 half{static_cast<float>(virtual_size.x) * 0.5f,
                    static_cast<float>(virtual_size.y) * 0.5f};

    Mat2x3 m = Mat2x3::trs({}, v.rotation, {v.zoom, v.zoom});
    const Vec2 t = half - m.apply_dir(centre);
    m.tx = t.x;
    m.ty = t.y;
    if (c.pixel_snap) {
        m.tx = std::floor(m.tx + 0.5f);
        m.ty = std::floor(m.ty + 0.5f);
    }
    v.world_to_screen = m;
    v.screen_to_world = m.inverse();
    // Can differ from the camera by under a pixel when snapped.
    v.center = v.screen_to_world.apply(half);

    // Cull rect: bounds of the screen corners in world space.
    const Vec2 corners[4] = {v.screen_to_world.apply({0, 0}),
                             v.screen_to_world.apply({half.x * 2, 0}),
                             v.screen_to_world.apply({half.x * 2, half.y * 2}),
                             v.screen_to_world.apply({0, half.y * 2})};
    Vec2 lo = corners[0], hi = corners[0];
    for (const Vec2 &p : corners) {
        lo = min(lo, p);
        hi = max(hi, p);
    }
    v.visible_world = Rect::from_min_max(lo, hi);
    v.view_id = 0;
    v.valid = true;
    return v;
}

} // namespace lam
