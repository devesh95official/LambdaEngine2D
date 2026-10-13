// collect_sprites, draw_lights, draw_debug and the F3 overlay. Sprites tie-break on entity
// index + 1 so equal keys draw in a stable order whatever the pool order is.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <lam/assets/asset_server.h>
#include <lam/core/debug.h>
#include <lam/core/fixed_string.h>
#include <lam/core/profiler.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/scalar.h>
#include <lam/render/camera.h>
#include <lam/render/fade.h>
#include <lam/render/font.h>
#include <lam/render/lighting.h>
#include <lam/render/passes.h>
#include <lam/render/render_plugin.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>
#include <lam/render/sprite.h>
#include <lam/render/virtual_screen.h>

#include "detail/systems.h"

namespace lam::detail {
namespace {

// Caches the last answer; most sprites share one page.
class TextureReady {
public:
    explicit TextureReady(const AssetServer *assets) : assets_(assets) {}
    bool operator()(AssetId<Texture> id) {
        if (!id) return true; // slot 0: the built-in image
        if (id == last_) return last_ready_;
        last_ = id;
        last_ready_ = assets_ == nullptr || assets_->get<Texture>(id) != nullptr;
        return last_ready_;
    }

private:
    const AssetServer *assets_;
    AssetId<Texture> last_;
    bool last_ready_ = true;
};

// Conservative half extents for culling, using the radius when rotated.
bool sprite_visible(const Sprite &s, const Transform2D &t, const Rect &area) {
    const TextureRegion &r = s.region;
    const float fw = r.size.x > 0.0f ? r.size.x : r.source.w;
    const float fh = r.size.y > 0.0f ? r.size.y : r.source.h;
    float ex =
        std::max(std::fabs(s.pivot.x), std::fabs(1.0f - s.pivot.x)) * fw * std::fabs(t.scale.x);
    float ey =
        std::max(std::fabs(s.pivot.y), std::fabs(1.0f - s.pivot.y)) * fh * std::fabs(t.scale.y);
    if (t.rotation != 0.0f) {
        const float radius = std::sqrt(ex * ex + ey * ey);
        ex = radius;
        ey = radius;
    }
    return t.position.x + ex >= area.x && t.position.x - ex <= area.x + area.w &&
           t.position.y + ey >= area.y && t.position.y - ey <= area.y + area.h;
}

bool circle_visible(Vec2 c, Vec2 radii, const Rect &area) {
    return c.x + radii.x >= area.x && c.x - radii.x <= area.x + area.w && c.y + radii.y >= area.y &&
           c.y - radii.y <= area.y + area.h;
}

const Font *default_font(const Ctx &ctx, const AssetServer *assets) {
    if (assets == nullptr) return nullptr;
    const RenderDefaults *d = ctx.try_res<RenderDefaults>();
    return d != nullptr ? assets->get<Font>(d->font) : nullptr;
}

float elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - since)
        .count();
}

} // namespace

void sys_collect_sprites(Ctx &ctx) {
    LAM_PROFILE_SCOPE("render.collect");
    const auto t0 = std::chrono::steady_clock::now();
    World &w = ctx.world();
    RenderQueue &q = ctx.res<RenderQueue>();
    RenderRuntime &rt = ctx.res<RenderRuntime>();
    const RenderConfig &cfg = ctx.res<RenderConfig>();
    const CameraView &view = ctx.local<CameraView>();
    const AssetServer *assets = ctx.try_res<AssetServer>();
    const float alpha = ctx.time().alpha;
    const bool lerp_transforms = alpha < 1.0f;
    const bool cull = cfg.cull;
    const Rect world_area = view.visible_world;
    const Rect screen_area{0.0f, 0.0f, static_cast<float>(view.virtual_size.x),
                           static_cast<float>(view.virtual_size.y)};
    const bool any_offsets = w.count<RenderOffset>() > 0;
    const bool any_fades = w.count<SpriteFade>() > 0;
    TextureReady ready(assets);
    std::uint32_t culled = 0, skipped = 0;

    const auto emit = [&](Entity e, const Sprite &s, Transform2D t) {
        if (any_offsets) {
            if (const RenderOffset *o = w.try_get<RenderOffset>(e)) {
                t.position += o->offset;
                t.scale = t.scale * o->scale;
                t.rotation += o->rotation;
            }
        }
        if (!ready(s.region.texture)) {
            ++skipped;
            return;
        }
        if (cull && !sprite_visible(s, t, is_screen_layer(s.layer) ? screen_area : world_area)) {
            ++culled;
            return;
        }
        const std::uint32_t tiebreak = e.index + 1u;
        if (any_fades) {
            if (const SpriteFade *f = w.try_get<SpriteFade>(e); f != nullptr && f->alpha < 1.0f) {
                Sprite faded = s;
                faded.tint = scale_alpha(s.tint, f->alpha);
                if (faded.tint.a == 0) return;
                q.sprite(faded, t, tiebreak);
                return;
            }
        }
        q.sprite(s, t, tiebreak);
    };

    if (lerp_transforms) {
        w.view<Sprite, Transform, PrevTransform>(Exclude<JustSpawned>{})
            .each([&](Entity e, Sprite &s, Transform &cur, PrevTransform &prev) {
                if ((s.flags & kHidden) != 0) return;
                emit(e, s, lerp(to_transform2d(prev), to_transform2d(cur), alpha));
            });
        w.view<Sprite, Transform>(With<JustSpawned>{})
            .each([&](Entity e, Sprite &s, Transform &cur) {
                if ((s.flags & kHidden) != 0) return;
                emit(e, s, to_transform2d(cur));
            });
        // No PrevTransform means no interpolation.
        w.view<Sprite, Transform>(Exclude<PrevTransform, JustSpawned>{})
            .each([&](Entity e, Sprite &s, Transform &cur) {
                if ((s.flags & kHidden) != 0) return;
                emit(e, s, to_transform2d(cur));
            });
    } else {
        w.view<Sprite, Transform>().each([&](Entity e, Sprite &s, Transform &cur) {
            if ((s.flags & kHidden) != 0) return;
            emit(e, s, to_transform2d(cur));
        });
    }

    // One layer below the sprite so shadows go under every actor on that layer.
    if (w.count<Shadow>() > 0) {
        const AssetId<Texture> shape = q.shape_texture();
        if (ready(shape)) {
            w.view<Shadow, Transform>().each([&](Entity e, Shadow &sh, Transform &) {
                std::uint8_t layer = static_cast<std::uint8_t>(kLayerWorld - 1);
                float depth_offset = 0.0f;
                if (const Sprite *s = w.try_get<Sprite>(e)) {
                    if ((s->flags & kHidden) != 0) return;
                    layer = s->layer > 0 ? static_cast<std::uint8_t>(s->layer - 1) : 0;
                    depth_offset = s->sort_offset;
                }
                const Vec2 pos = interpolated(w, e, alpha).position;
                const Vec2 c = pos + sh.offset;
                if (cull && !circle_visible(c, sh.radii,
                                            is_screen_layer(layer) ? screen_area : world_area)) {
                    ++culled;
                    return;
                }
                Color color = sh.color;
                if (any_fades) {
                    if (const SpriteFade *f = w.try_get<SpriteFade>(e))
                        color = scale_alpha(color, f->alpha);
                }
                if (color.a == 0) return;
                q.ellipse(c, sh.radii, color, layer, pos.y + depth_offset);
            });
        }
    }

    // Labels on screen layers follow the entity at a fixed pixel size.
    if (w.count<Text>() > 0 && assets != nullptr) {
        const Font *fallback = default_font(ctx, assets);
        w.view<Text, Transform>().each([&](Entity e, Text &text, Transform &) {
            if (text.value.empty()) return;
            const Font *font = text.style.font ? assets->get<Font>(text.style.font) : fallback;
            if (font == nullptr || !ready(font->page)) {
                ++skipped;
                return;
            }
            const Vec2 world_pos = interpolated(w, e, alpha).position + text.offset;
            Vec2 pos = is_screen_layer(text.style.layer) ? round(world_to_screen(view, world_pos))
                                                         : world_pos;
            const Vec2 size = measure_text(*font, text.value.view(), text.style.size);
            if (text.align == TextAlign::Center) pos.x -= std::floor(size.x * 0.5f);
            if (text.align == TextAlign::Right) pos.x -= size.x;
            const Rect area = is_screen_layer(text.style.layer) ? screen_area : world_area;
            if (cull && !Rect{pos.x, pos.y, size.x + 1.0f, size.y + 1.0f}.overlaps(area)) {
                ++culled;
                return;
            }
            draw_text(q, *font, text.value.view(), pos, text.style, pos.y);
        });
    }

    rt.culled += culled;
    rt.skipped_unloaded += skipped;
    rt.collect_ns += static_cast<std::uint64_t>(elapsed_ms(t0) * 1.0e6f);
}

bool lighting_enabled(const Ctx &ctx) {
    const Lighting *l = ctx.try_local<Lighting>();
    return l != nullptr && l->enabled;
}

void sys_draw_lights(Ctx &ctx) {
    World &w = ctx.world();
    const Lighting &lighting = ctx.local<Lighting>();
    if (!lighting.enabled) return;
    RenderPasses &passes = ctx.res<RenderPasses>();
    ensure_light_pass(passes, lighting.resolution_scale);
    RenderQueue &q = ctx.res<RenderQueue>();
    const CameraView &view = ctx.local<CameraView>();
    const RenderConfig &cfg = ctx.res<RenderConfig>();
    // Ambient first, so each world can have its own.
    q.rect(view.visible_world.expanded(2.0f), lighting.ambient.with_alpha(255), kLayerLights,
           -524288.0f);
    const float t = static_cast<float>(ctx.time().real_seconds);
    const float alpha = ctx.time().alpha;
    std::uint32_t culled = 0;
    w.view<Light2D, Transform>().each([&](Entity e, Light2D &light, Transform &) {
        const LightStamp s = light_stamp(light, t);
        if (!(s.radius > 0.0f) || s.color.a == 0) return;
        const Vec2 c = interpolated(w, e, alpha).position + light.offset;
        if (cfg.cull && !circle_visible(c, {s.radius, s.radius}, view.visible_world)) {
            ++culled;
            return;
        }
        q.light(c, s.radius, s.color, kMaterialAdditive, kLayerLights);
    });
    ctx.res<RenderRuntime>().culled += culled;
}

void sys_draw_debug(Ctx &ctx) {
    if constexpr (!LAM_DEV) {
        (void)ctx;
        return;
    } else {
        const DebugDraw *dd = ctx.try_local<DebugDraw>();
        if (dd == nullptr || dd->prims().empty()) return;
        RenderQueue &q = ctx.res<RenderQueue>();
        const CameraView &view = ctx.local<CameraView>();
        const Font *font = default_font(ctx, ctx.try_res<AssetServer>());
        const auto to_screen = [&](const DebugPrim &p, Vec2 v) {
            return p.screen ? v : world_to_screen(view, v);
        };
        for (const DebugPrim &p : dd->prims()) {
            switch (p.kind) {
            case DebugPrimKind::Line:
                q.line(to_screen(p, p.a), to_screen(p, p.b), 1.0f, p.color, kLayerDebug);
                break;
            case DebugPrimKind::Rect: {
                const Vec2 c[4] = {to_screen(p, p.a), to_screen(p, {p.a.x + p.b.x, p.a.y}),
                                   to_screen(p, p.a + p.b), to_screen(p, {p.a.x, p.a.y + p.b.y})};
                for (int i = 0; i < 4; ++i)
                    q.line(c[i], c[(i + 1) % 4], 1.0f, p.color, kLayerDebug);
                break;
            }
            case DebugPrimKind::Circle: {
                constexpr int kSegments = 20;
                Vec2 prev = to_screen(p, p.a + Vec2{p.radius, 0.0f});
                for (int i = 1; i <= kSegments; ++i) {
                    const float a = kTau * static_cast<float>(i) / static_cast<float>(kSegments);
                    const Vec2 next = to_screen(p, p.a + Vec2{det_cos(a), det_sin(a)} * p.radius);
                    q.line(prev, next, 1.0f, p.color, kLayerDebug);
                    prev = next;
                }
                break;
            }
            case DebugPrimKind::Text:
                if (font != nullptr) {
                    TextStyle style;
                    style.color = p.color;
                    style.layer = kLayerDebug;
                    draw_text(q, *font, p.text.view(), round(to_screen(p, p.a)), style);
                }
                break;
            }
        }
    }
}

void sys_draw_overlay(Ctx &ctx) {
    if constexpr (!LAM_DEV) {
        (void)ctx;
        return;
    } else {
        RenderRuntime &rt = ctx.res<RenderRuntime>();
        // Wall clock, because frame_dt is fixed under --step.
        const double now = ctx.time().real_seconds;
        const float dt = rt.fps_last_real >= 0.0 ? static_cast<float>(now - rt.fps_last_real) : ctx.time().frame_dt;
        rt.fps_last_real = now;
        if (dt > 0.0f && is_finite(dt)) {
            const float fps = 1.0f / dt;
            rt.fps = rt.fps > 0.0f ? rt.fps + (fps - rt.fps) * 0.1f : fps;
        }
        const DebugOverlay *overlay = ctx.try_res<DebugOverlay>();
        if (overlay == nullptr || !overlay->visible) return;
        const AssetServer *assets = ctx.try_res<AssetServer>();
        const Font *font = default_font(ctx, assets);
        if (font == nullptr) return;
        RenderQueue &q = ctx.res<RenderQueue>();
        App &app = ctx.app();

        // Format the lines first to size the panel.
        constexpr std::size_t kMaxLines = 40;
        std::array<FixedString<96>, kMaxLines> lines;
        std::size_t n = 0;
        const RenderStats &st = ctx.res<RenderStats>();
        std::size_t entities = 0;
        for (const WorldEntry &e : app.world_stack())
            if (e.world != nullptr) entities += e.world->entity_count();
        format_to(lines[n++], "FPS {:.1f} ({:.2f} ms)", rt.fps,
                  rt.fps > 0.0f ? 1000.0f / rt.fps : 0.0f);
        format_to(lines[n++], "draws {} batches {} quads {}", st.draw_calls, st.batches, st.quads);
        format_to(lines[n++], "culled {} unloaded {} views {}", st.culled, st.skipped_unloaded,
                  st.views);
        format_to(lines[n++], "entities {} worlds {}", entities, app.world_stack().size());
        format_to(lines[n++], "cpu collect {:.2f} plan {:.2f} submit {:.2f} swap {:.2f}",
                  st.cpu_collect_ms, st.cpu_plan_ms, st.cpu_upload_ms, st.gpu_swap_ms);
        for (const DebugOverlay::Stat &s : overlay->stats()) {
            if (n >= kMaxLines) break;
            format_to(lines[n++], "{} {:.3f}", s.label, s.value);
        }
        for (const FixedString<96> &l : overlay->lines()) {
            if (n >= kMaxLines) break;
            lines[n++] = l;
        }
        if (const Profiler *prof = ctx.try_res<Profiler>()) {
            // 8 slowest timings.
            const std::span<const StageTiming> t = prof->timings();
            std::array<bool, 256> shown{};
            for (int k = 0; k < 8 && n < kMaxLines; ++k) {
                std::size_t best = t.size();
                for (std::size_t i = 0; i < t.size() && i < shown.size(); ++i) {
                    if (shown[i]) continue;
                    if (best == t.size() || t[i].ms > t[best].ms) best = i;
                }
                if (best == t.size()) break;
                shown[best] = true;
                format_to(lines[n++], "{:>7.3f} ms {}", t[best].ms, t[best].name);
            }
        }

        const auto &views = q.views();
        if (views.empty()) return;
        q.set_view(static_cast<std::uint8_t>(views.size() - 1)); // above every world
        q.set_clip(0);
        float width = 0.0f;
        for (std::size_t i = 0; i < n; ++i)
            width = std::max(width, measure_text(*font, lines[i].view(), 0.0f).x);
        const float line_h = font->line_height + 1.0f;
        q.rect({2.0f, 2.0f, width + 6.0f, static_cast<float>(n) * line_h + 4.0f},
               Color{0, 0, 0, 170}, kLayerDebug);
        TextStyle style;
        style.layer = kLayerDebug;
        style.color = Color{235, 235, 200, 255};
        for (std::size_t i = 0; i < n; ++i)
            draw_text(q, *font, lines[i].view(), {5.0f, 4.0f + static_cast<float>(i) * line_h},
                      style);
    }
}

} // namespace lam::detail
