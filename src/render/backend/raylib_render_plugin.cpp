// RaylibRenderPlugin: decoder and loaders, texture uploads, render_submit and the swap timer.
// The platform owns BeginDrawing/EndDrawing and the swap; we only draw in between.
#include <lam/render/render_plugin.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include <raylib.h>
#include <rlgl.h>

#include <lam/assets/asset_server.h>
#include <lam/core/log.h>
#include <lam/core/platform.h>
#include <lam/render/atlas.h>
#include <lam/render/font.h>
#include <lam/render/passes.h>
#include <lam/render/render_plan.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>
#include <lam/render/texture.h>
#include <lam/render/virtual_screen.h>

#include "backend/gpu.h"
#include "backend/rl_loaders.h"
#include "detail/render_internal.h"

namespace lam {
namespace detail {
namespace {

struct RlRenderState {
    GpuBatcher batcher;
    std::vector<GpuTexture> textures; // by draw slot
    bool gpu = false;
    std::chrono::steady_clock::time_point submit_end{};
    bool have_submit_end = false;
    std::string screenshot_path;
    bool screenshot_done = false;
    std::uint64_t upload_driver_allocs = 0;
    void release() {
        textures.clear();
        batcher.release();
        gpu = false;
    }
};

void set_texture_params(unsigned int id, TextureFilter filter) {
    const int f =
        filter == TextureFilter::Bilinear ? RL_TEXTURE_FILTER_LINEAR : RL_TEXTURE_FILTER_NEAREST;
    rlTextureParameters(id, RL_TEXTURE_MIN_FILTER, f);
    rlTextureParameters(id, RL_TEXTURE_MAG_FILTER, f);
    rlTextureParameters(id, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
    rlTextureParameters(id, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
}

void upload_pixels(GpuTexture &g, const void *rgba, int w, int h, TextureFilter filter) {
    if (g.tex.valid() && g.width == w && g.height == h) {
        rlUpdateTexture(g.tex.get(), 0, 0, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, rgba);
    } else {
        g.tex.reset(rlLoadTexture(rgba, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1));
        g.width = w;
        g.height = h;
    }
    if (g.tex.valid()) set_texture_params(g.tex.get(), filter);
}

void upload_texture(RlRenderState &st, RenderBackendStats &stats, AssetServer &assets,
                    AssetId<Texture> id) {
    Texture *t = assets.get_mut<Texture>(id);
    if (t == nullptr || t->width <= 0 || t->height <= 0 || id.index >= 65536u) return;
    const std::size_t slot = id.index;
    if (st.textures.size() <= slot) st.textures.resize(slot + 1);
    GpuTexture &g = st.textures[slot];
    // A reload starts a fresh Texture at version 1, so also check the AssetServer version.
    const std::uint32_t asset_version = assets.version(id);
    if (g.tex.valid() && g.generation == id.generation && g.version == t->version &&
        g.asset_version == asset_version)
        return;
    if (!t->pixels.has_pixels()) {
        if (t->keep_cpu) {
            ensure_page_pixels(*t); // e.g. the built-in font page
        } else {
            // Size but no pixels: upload transparent texels.
            std::vector<std::uint8_t> zero(
                static_cast<std::size_t>(t->width) * static_cast<std::size_t>(t->height) * 4, 0);
            {
                DriverAllocScope gl(st.upload_driver_allocs);
                upload_pixels(g, zero.data(), t->width, t->height, t->filter);
            }
            ++stats.texture_uploads;
            g.generation = id.generation;
            g.version = t->version;
            g.asset_version = asset_version;
            return;
        }
    }
    // GL reads width * height * 4 bytes. If the size doesn't match, keep the old texture.
    const std::size_t need =
        static_cast<std::size_t>(t->width) * static_cast<std::size_t>(t->height) * 4;
    if (t->pixels.rgba.size() != need || t->pixels.width != t->width ||
        t->pixels.height != t->height) {
        LAM_LOG_ERROR("render: texture {} has {}x{} pixels ({} bytes) for a {}x{} texture; not "
                      "uploaded",
                      id.index, t->pixels.width, t->pixels.height, t->pixels.rgba.size(), t->width,
                      t->height);
        return;
    }
    {
        DriverAllocScope gl(st.upload_driver_allocs);
        upload_pixels(g, t->pixels.rgba.data(), t->width, t->height, t->filter);
    }
    ++stats.texture_uploads;
    g.generation = id.generation;
    g.version = t->version;
    g.asset_version = asset_version;
    if (!t->keep_cpu) std::vector<std::uint8_t>().swap(t->pixels.rgba);
}

// Safe to run twice per frame; render_submit calls it again.
void process_texture_events(RlRenderState &st, RenderBackendStats &stats, AssetServer &assets,
                            std::span<const AssetEvent<Texture>> events) {
    for (const AssetEvent<Texture> &ev : events) {
        switch (ev.kind) {
        case AssetEvent<Texture>::Kind::Loaded:
        case AssetEvent<Texture>::Kind::Modified:
            upload_texture(st, stats, assets, ev.id);
            break;
        case AssetEvent<Texture>::Kind::Removed:
        case AssetEvent<Texture>::Kind::Failed:
            if (ev.id.index != 0 && ev.id.index < st.textures.size() &&
                st.textures[ev.id.index].generation == ev.id.generation) {
                DriverAllocScope gl(st.upload_driver_allocs);
                st.textures[ev.id.index] = GpuTexture{};
            }
            break;
        }
    }
}

void sys_texture_upload(Ctx &ctx) {
    RlRenderState &st = ctx.res<RlRenderState>();
    if (!st.gpu) return;
    process_texture_events(st, ctx.res<RenderBackendStats>(), ctx.res<AssetServer>(),
                           ctx.read<AssetEvent<Texture>>());
    // A reloaded shader may reuse a program id, so drop the cached locations.
    for (const AssetEvent<Shader> &ev : ctx.read<AssetEvent<Shader>>()) {
        if (ev.kind != AssetEvent<Shader>::Kind::Loaded) st.batcher.forget_custom_programs();
    }
}

void sys_render_submit(Ctx &ctx) {
    RlRenderState &st = ctx.res<RlRenderState>();
    if (!st.gpu) return;
    const auto t0 = std::chrono::steady_clock::now();
    // Again, in case a slot was freed and reused since texture_upload ran.
    process_texture_events(st, ctx.res<RenderBackendStats>(), ctx.res<AssetServer>(),
                           ctx.read<AssetEvent<Texture>>());
    App &app = ctx.app();
    const Time &time = ctx.time();
    const RenderQueue &queue = ctx.res<RenderQueue>();
    ScreenCapture &capture = ctx.res<ScreenCapture>();
    SubmitFrame f;
    f.plan = &ctx.res<RenderPlan>();
    f.views = queue.views();
    f.clips = queue.clips();
    f.passes = ctx.try_res<RenderPasses>();
    f.materials = ctx.try_res<Materials>();
    f.assets = ctx.try_res<AssetServer>();
    f.layout = ctx.res<VirtualLayout>();
    f.clear_color = ctx.res<RenderConfig>().clear_color;
    f.time_seconds = static_cast<float>(time.real_seconds);
    const int every = app.options().draw_every;
    f.draw_scene = every <= 1 || time.frame % static_cast<std::uint64_t>(every) == 0;
    f.capture = capture.request;
    std::uint32_t skipped = 0;
    const std::uint32_t draws =
        st.batcher.submit(f, st.textures, &skipped, capture.request ? &capture.image : nullptr);
    if (capture.request) {
        capture.request = false;
        capture.ready = capture.image.has_pixels();
        capture.frame = time.frame;
    }
    RenderStats &stats = ctx.res<RenderStats>();
    stats.draw_calls = draws;
    stats.skipped_unloaded += skipped;
    RenderBackendStats &backend = ctx.res<RenderBackendStats>();
    ++backend.frames;
    backend.driver_allocations = st.batcher.driver_allocations() + st.upload_driver_allocs;
    // --screenshot FILE: the last frame (--frames N), or the frame in which quit() was called.
    if (!st.screenshot_path.empty() && !st.screenshot_done) {
        const int frames = app.options().frames;
        const bool last =
            frames > 0 ? time.frame + 1 >= static_cast<std::uint64_t>(frames) : app.quitting();
        if (last) {
            st.screenshot_done = true;
            if (st.batcher.screenshot(st.screenshot_path.c_str())) {
                LAM_LOG_INFO("render: screenshot written to {}", st.screenshot_path);
            } else {
                LAM_LOG_ERROR("render: cannot write the screenshot {}", st.screenshot_path);
            }
        }
    }
    st.submit_end = std::chrono::steady_clock::now();
    st.have_submit_end = true;
    stats.cpu_upload_ms = std::chrono::duration<float, std::milli>(st.submit_end - t0).count();
}

void sys_render_swap_timer(Ctx &ctx) {
    RlRenderState &st = ctx.res<RlRenderState>();
    if (!st.have_submit_end) return;
    st.have_submit_end = false;
    ctx.res<RenderStats>().gpu_swap_ms =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - st.submit_end)
            .count();
}

// RenderPlugin only reserved space for the built-in font; blit the pixels now.
void fill_builtin_font(App &app, AssetServer &assets) {
    const RenderDefaults *defaults = app.try_res<RenderDefaults>();
    if (defaults == nullptr) return;
    const Font *font = assets.get<Font>(defaults->font);
    if (font == nullptr || !font->page) return;
    const Glyph *first = font->find(U' ');
    if (first == nullptr) return;
    Texture *page = assets.get_mut<Texture>(font->page);
    if (page == nullptr) return;
    ensure_page_pixels(*page);
    blit_into(*page, builtin_font_image(),
              {static_cast<int>(first->source.x), static_cast<int>(first->source.y)});
    ++page->version;
    assets.mark_modified(font->page);
}

void add_once(App &app, Stage stage, SystemFn fn, std::string_view name, int order) {
    SystemDesc d;
    d.stage = stage;
    d.fn = fn;
    d.name = name;
    d.order = order;
    d.scope = SystemScope::Once;
    app.add_system(d);
}

} // namespace
} // namespace detail

void RaylibRenderPlugin::build(App &app) {
    using namespace detail;
    app.require_plugin("lam.render", name());
    set_image_decoder(raylib_image_decoder());
    AssetServer &assets = app.res<AssetServer>();
    assets.register_loader<Font>(make_raylib_ttf_loader());
    assets.register_loader<Shader>(make_raylib_shader_loader());
    RlRenderState &st = app.insert_resource<RlRenderState>();
    RenderBackendStats &backend = app.insert_resource<RenderBackendStats>();
    st.gpu = app.platform().has_gpu();
    if (!st.gpu) {
        LAM_LOG_INFO("render: no GPU context; the raylib backend only decodes images and fonts");
        return;
    }
    const char *env = std::getenv("LAM_RENDER_RLGL_BATCH");
    const bool fallback = config_.rlgl_batch || (env != nullptr && env[0] == '1');
    if (!st.batcher.init(fallback)) {
        st.gpu = false;
        return;
    }
    backend.rlgl_batch = st.batcher.rlgl_batch();
    st.screenshot_path = app.options().screenshot;
    st.textures.reserve(1024);
    st.textures.resize(1);
    const ImageData &builtin = builtin_image();
    upload_pixels(st.textures[0], builtin.rgba.data(), builtin.width, builtin.height,
                  TextureFilter::Point);
    ++backend.texture_uploads;
    st.textures[0].version = 1;
    fill_builtin_font(app, assets);
    add_once(app, Stage::FrameBegin, sys_texture_upload, "texture_upload", -2500);
    add_once(app, Stage::FrameEnd, sys_render_submit, "render_submit", -500);
    add_once(app, Stage::FrameEnd, sys_render_swap_timer, "render_swap_timer", 10001);
}

void RaylibRenderPlugin::shutdown(App &app) {
    if (auto *st = app.try_res<detail::RlRenderState>()) st->release();
    set_image_decoder(nullptr);
}

} // namespace lam
