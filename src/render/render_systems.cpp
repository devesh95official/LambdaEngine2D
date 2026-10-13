// Virtual layout, fades, cameras, views, planning and the end-of-frame clear. Presentation only;
// none of this touches hashed state.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <span>

#include <lam/core/debug.h>
#include <lam/core/platform.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/scalar.h>
#include <lam/render/atlas.h>
#include <lam/render/camera.h>
#include <lam/render/camera_math.h>
#include <lam/render/fade.h>
#include <lam/render/passes.h>
#include <lam/render/render_plan.h>
#include <lam/render/render_plugin.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include <lam/render/virtual_screen.h>

#include "detail/systems.h"

namespace lam::detail {
namespace {

// Extra revealers are ignored.
constexpr int kMaxRevealers = 32;

Vec2 virtual_size_of(const Ctx &ctx) {
    const VirtualLayout *l = ctx.try_res<VirtualLayout>();
    return l != nullptr ? to_vec2(l->virtual_size) : to_vec2(ctx.app().config().virtual_size);
}

// A bad offset from save data falls back to the target itself.
Vec2 follow_offset(const CameraFollow &f) {
    return is_finite(f.offset) ? f.offset : Vec2{};
}

IVec2 virtual_isize_of(const Ctx &ctx) {
    const VirtualLayout *l = ctx.try_res<VirtualLayout>();
    return l != nullptr ? l->virtual_size : ctx.app().config().virtual_size;
}

// Worlds without a camera: world pixels are virtual pixels.
CameraView default_view(IVec2 virtual_size, bool pixel_perfect) {
    Camera2D c;
    c.position = {static_cast<float>(virtual_size.x) * 0.5f,
                  static_cast<float>(virtual_size.y) * 0.5f};
    return make_camera_view(c, virtual_size, {}, pixel_perfect);
}

} // namespace

float effective_zoom(float zoom, bool pixel_perfect) {
    if (!is_finite(zoom) || !(zoom > 0.0f)) return 1.0f;
    if (!pixel_perfect) return zoom;
    if (zoom >= 1.0f) return std::floor(zoom + 0.5f);
    const float n = std::floor(1.0f / zoom + 0.5f);
    return n >= 1.0f ? 1.0f / n : 1.0f;
}

bool snaps_zoom(const App &app) {
    const RenderConfig *cfg = const_cast<App &>(app).try_res<RenderConfig>();
    const ScaleMode m = cfg != nullptr ? cfg->scale_mode : ScaleMode::PixelPerfect;
    return m == ScaleMode::PixelPerfect || m == ScaleMode::PixelExpand;
}

void sys_virtual_layout_update(Ctx &ctx) {
    const RenderConfig &cfg = ctx.res<RenderConfig>();
    VirtualLayout &l = ctx.res<VirtualLayout>();
    l = compute_virtual_layout(ctx.app().platform().window().framebuffer,
                               ctx.app().config().virtual_size, cfg.scale_mode);
}

void sys_atlas_shape_page(Ctx &ctx) {
    ctx.res<RenderQueue>().set_shape_texture(ctx.res<AtlasSet>().shape_page());
}

void sys_fade_occluders(Ctx &ctx) {
    World &w = ctx.world();
    if (w.count<FadeOccluder>() == 0) return;
    const float alpha = ctx.time().alpha;
    std::array<Vec2, kMaxRevealers> feet{};
    int revealers = 0;
    w.view<Transform>(With<FadeRevealer>{}).each([&](Entity e, Transform &) {
        if (revealers < kMaxRevealers)
            feet[static_cast<std::size_t>(revealers++)] = interpolated(w, e, alpha).position;
    });
    w.view<FadeOccluder, Transform>().each([&](Entity e, FadeOccluder &occ, Transform &) {
        const Vec2 pos = interpolated(w, e, alpha).position;
        const Rect top = occluder_rect(occ, pos);
        bool hidden = false;
        for (int i = 0; i < revealers && !hidden; ++i)
            hidden = hidden_behind(top, occ.inset, pos.y, feet[static_cast<std::size_t>(i)]);
        const float target = hidden ? std::clamp(occ.faded_alpha, 0.0f, 1.0f) : 1.0f;
        if (SpriteFade *f = w.try_get<SpriteFade>(e)) {
            f->target = target;
        } else {
            w.commands().add<SpriteFade>(e, SpriteFade{1.0f, target, 0.2f});
        }
    });
}

void sys_sprite_fades(Ctx &ctx) {
    World &w = ctx.world();
    const float dt = ctx.time().frame_dt;
    w.view<SpriteFade>().each(
        [&](Entity, SpriteFade &f) { f.alpha = step_fade(f.alpha, f.target, f.seconds, dt); });
}

void sys_camera_follow(Ctx &ctx) {
    World &w = ctx.world();
    if (w.count<CameraFollow>() == 0) return;
    const Time &time = ctx.time();
    const float dt = time.frame_dt;
    const float tick_dt = time.tick_dt > 0.0f ? time.tick_dt : 1.0f / 60.0f;
    const bool pp = snaps_zoom(ctx.app());
    const Vec2 vsize = virtual_size_of(ctx);
    w.view<Camera2D, CameraFollow>().each([&](Entity e, Camera2D &cam, CameraFollow &follow) {
        const Entity target = follow.target;
        const Transform *cur = w.try_get<Transform>(target);
        if (cur == nullptr) return;
        // Follow the interpolated position, since that's what is drawn.
        const Vec2 aim_target =
            interpolated(w, target, time.alpha).position + follow_offset(follow);
        Vec2 velocity;
        if (const PrevTransform *prev = w.try_get<PrevTransform>(target);
            prev != nullptr && !w.has<JustSpawned>(target)) {
            velocity = (cur->position - prev->position) / tick_dt;
        }
        CameraFollowState *state = w.try_get<CameraFollowState>(e);
        CameraFollowState fresh;
        const bool is_new = state == nullptr;
        if (is_new) state = &fresh;
        const bool jumped = follow.snap_distance > 0.0f && state->tracking &&
                            distance_sq(aim_target, state->last_target) >
                                follow.snap_distance * follow.snap_distance;
        if (!state->tracking || follow.snap_next || jumped || !is_finite(cam.position)) {
            state->lookahead = {};
            cam.position = aim_target;
            follow.snap_next = false;
        } else {
            state->lookahead = camera_lookahead_step(state->lookahead, velocity, follow, dt);
            cam.position =
                camera_follow_point(cam.position, aim_target + state->lookahead, follow, dt);
        }
        if (const CameraBounds *b = w.try_get<CameraBounds>(e); b != nullptr && b->enabled) {
            const float zoom = effective_zoom(cam.zoom, pp);
            cam.position = clamp_to_bounds(cam.position, vsize / zoom, b->world);
        }
        state->last_target = aim_target;
        state->tracking = true;
        if (is_new) w.commands().add<CameraFollowState>(e, fresh);
    });
}

void sys_camera_shake(Ctx &ctx) {
    World &w = ctx.world();
    const float dt = ctx.time().frame_dt;
    // Every event adds to every shake; add_trauma clamps after each one.
    const std::span<const ShakeAt> events = ctx.read<ShakeAt>();
    w.view<CameraShake>().each([&](Entity, CameraShake &s) {
        for (const ShakeAt &ev : events)
            add_trauma(s, ev.trauma);
        decay_trauma(s, dt);
    });
}

void sys_camera_view(Ctx &ctx) {
    World &w = ctx.world();
    CameraView &view = ctx.local<CameraView>();
    const IVec2 vsize = virtual_isize_of(ctx);
    const bool pp = snaps_zoom(ctx.app());
    Entity best;
    const Camera2D *best_cam = nullptr;
    w.view<Camera2D>().each([&](Entity e, Camera2D &c) {
        if (best_cam == nullptr || c.priority > best_cam->priority ||
            (c.priority == best_cam->priority && e.index < best.index)) {
            best = e;
            best_cam = &c;
        }
    });
    const std::uint8_t view_id = view.view_id;
    if (best_cam == nullptr) {
        view = default_view(vsize, pp);
    } else {
        Camera2D cam = *best_cam;
        if (const CameraBounds *b = w.try_get<CameraBounds>(best); b != nullptr && b->enabled) {
            cam.position = clamp_to_bounds(cam.position,
                                           to_vec2(vsize) / effective_zoom(cam.zoom, pp), b->world);
        }
        Vec2 shake;
        if (const CameraShake *s = w.try_get<CameraShake>(best))
            shake = camera_shake_offset(*s, best.index);
        view = make_camera_view(cam, vsize, shake, pp);
    }
    view.view_id = view_id;
}

void sys_begin_view(Ctx &ctx) {
    CameraView &view = ctx.local<CameraView>();
    if (!view.valid) {
        const std::uint8_t id = view.view_id;
        view = default_view(virtual_isize_of(ctx), snaps_zoom(ctx.app()));
        view.view_id = id;
    }
    view.view_id = ctx.res<RenderQueue>().begin_view(view);
}

void sys_resume_view(Ctx &ctx) {
    RenderQueue &q = ctx.res<RenderQueue>();
    const CameraView &view = ctx.local<CameraView>();
    if (view.view_id < q.views().size()) q.set_view(view.view_id);
}

void sys_render_plan(Ctx &ctx) {
    const auto t0 = std::chrono::steady_clock::now();
    const RenderQueue &queue = ctx.res<RenderQueue>();
    RenderPlan &plan = ctx.res<RenderPlan>();
    const RenderPasses *passes = ctx.try_res<RenderPasses>();
    plan_batches(queue, passes, plan.scratch, plan.batches, plan.staging);
    plan.frame = ctx.time().frame;

    RenderStats &st = ctx.res<RenderStats>();
    RenderRuntime &rt = ctx.res<RenderRuntime>();
    st.batches = static_cast<std::uint32_t>(plan.batches.size());
    st.draw_calls = st.batches;
    st.quads = static_cast<std::uint32_t>(queue.size());
    st.culled = rt.culled;
    st.skipped_unloaded = rt.skipped_unloaded;
    st.views = static_cast<std::uint32_t>(queue.views().size());
    std::uint32_t switches = 0, target_runs = 0;
    int last_texture = -1, last_target = -1;
    for (const Batch &b : plan.batches) {
        if (b.target != last_target) {
            ++target_runs;
            last_target = b.target;
        }
        if (b.composite_source != 0) continue;
        if (last_texture >= 0 && b.texture != last_texture) ++switches;
        last_texture = b.texture;
    }
    st.texture_switches = switches;
    st.passes = target_runs;
    st.cpu_collect_ms = static_cast<float>(static_cast<double>(rt.collect_ns) / 1.0e6);
    st.cpu_upload_ms = 0.0f;
    rt.collect_ns = 0;
    rt.culled = 0;
    rt.skipped_unloaded = 0;
    const auto t1 = std::chrono::steady_clock::now();
    st.cpu_plan_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
}

void sys_render_clear(Ctx &ctx) {
    ctx.res<RenderQueue>().clear();
    App &app = ctx.app();
    if (DebugDraw *d = app.main_world().resources().try_get<DebugDraw>()) d->clear();
    for (const WorldEntry &e : app.world_stack()) {
        if (e.world == nullptr || e.world == &app.main_world()) continue;
        if (DebugDraw *d = e.world->resources().try_get<DebugDraw>()) d->clear();
    }
}

} // namespace lam::detail
