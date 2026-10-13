#pragma once
// Render systems, defined in render_systems.cpp and collect.cpp.
#include <cstdint>
#include <lam/core/app.h>
#include <lam/math/vec2.h>
namespace lam::detail {
// Counters render_plan copies into RenderStats each frame.
struct RenderRuntime {
    std::uint64_t collect_ns = 0;
    std::uint32_t culled = 0;
    std::uint32_t skipped_unloaded = 0;
    float fps = 0; // smoothed, for the overlay
    double fps_last_real = -1; // real time, since frame_dt is fixed under --step
    bool warned_no_light_target = false;
};
// Same rule as make_camera_view: bad zoom -> 1, pixel_perfect snaps to n or 1/n.
float effective_zoom(float zoom, bool pixel_perfect);
bool snaps_zoom(const App &app);

void sys_virtual_layout_update(Ctx &ctx); // FrameBegin Once -9000
void sys_atlas_shape_page(Ctx &ctx);      // FrameBegin Once -2400
void sys_fade_occluders(Ctx &ctx);        // Update PerWorld -1400
void sys_sprite_fades(Ctx &ctx);          // Update PerWorld -1400 (after)
void sys_camera_follow(Ctx &ctx);         // Update PerWorld -1300
void sys_camera_shake(Ctx &ctx);          // Update PerWorld -1200
void sys_camera_view(Ctx &ctx);           // Update PerWorld -1100
void sys_begin_view(Ctx &ctx);            // Render PerWorld -2000
void sys_collect_sprites(Ctx &ctx);       // Render PerWorld -900
void sys_draw_lights(Ctx &ctx);           // Render PerWorld -700
bool lighting_enabled(const Ctx &ctx);    // run_if of draw_lights
void sys_resume_view(Ctx &ctx);           // RenderUI PerWorld -2000
void sys_draw_debug(Ctx &ctx);            // RenderUI PerWorld +2000
void sys_draw_overlay(Ctx &ctx);          // RenderUI Once +2000
void sys_render_plan(Ctx &ctx);           // FrameEnd Once -1000
void sys_render_clear(Ctx &ctx);          // FrameEnd Once +9000
} // namespace lam::detail
