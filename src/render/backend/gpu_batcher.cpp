// Sprite batcher on rlgl. rlgl has no buffer orphaning and no base-vertex draws, so we keep a
// 3-frame ring of VBO segments (never rewriting one the GPU may still read) and re-point the VAO
// attributes at each batch's first vertex so a single u16 index buffer works for everything.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/log.h>
#include <lam/render/shader.h>
#include <lam/rl/convert.h>

#include "backend/gpu.h"

namespace lam::detail {
namespace {

constexpr std::uint32_t kSegmentQuads = kMaxQuadsPerBatch;
constexpr int kStride = static_cast<int>(sizeof(QuadVertex));
constexpr int kSegmentBytes = static_cast<int>(kSegmentQuads) * 4 * kStride;
const Material kAlphaMaterial{}; // used when there's no Materials resource

static_assert(offsetof(QuadVertex, pos) == 0 && offsetof(QuadVertex, uv) == 8 &&
                  offsetof(QuadVertex, color) == 16,
              "QuadVertex layout: pos, uv, color");

// Orthographic projection with +y down: x 0..w, y 0..h map to the whole viewport.
::Matrix ortho_y_down(float w, float h) {
    ::Matrix m{};
    m.m0 = 2.0f / w;
    m.m5 = -2.0f / h;
    m.m10 = -1.0f;
    m.m12 = -1.0f;
    m.m13 = 1.0f;
    m.m15 = 1.0f;
    return m;
}

int gl_filter(TextureFilter f) {
    return f == TextureFilter::Bilinear ? RL_TEXTURE_FILTER_LINEAR : RL_TEXTURE_FILTER_NEAREST;
}

std::string build_or_empty(ShaderStage stage, std::string_view body) {
    Result<std::string> s = build_shader_source(current_shader_dialect(), stage, body, "engine");
    return s ? std::move(s.value()) : std::string();
}

void set_vec4(int loc, float x, float y, float z, float w) {
    if (loc < 0) return;
    const float v[4] = {x, y, z, w};
    rlSetUniform(loc, v, RL_SHADER_UNIFORM_VEC4, 1);
}

void set_vec2(int loc, float x, float y) {
    if (loc < 0) return;
    const float v[2] = {x, y};
    rlSetUniform(loc, v, RL_SHADER_UNIFORM_VEC2, 1);
}

void set_float(int loc, float x) {
    if (loc < 0) return;
    rlSetUniform(loc, &x, RL_SHADER_UNIFORM_FLOAT, 1);
}

} // namespace

unsigned int GpuBatcher::compile(const char *vs, const char *fs) {
    const unsigned int id = rlLoadShaderProgram(vs, fs);
    return id == rlGetShaderIdDefault() ? 0u : id;
}

void GpuBatcher::setup_program(GpuProgram &p) {
    p.loc_mvp = rlGetLocationUniform(p.id, "mvp");
    p.loc_view_x = rlGetLocationUniform(p.id, "u_view_x");
    p.loc_view_y = rlGetLocationUniform(p.id, "u_view_y");
    p.loc_snap = rlGetLocationUniform(p.id, "u_snap");
    p.loc_inv_size = rlGetLocationUniform(p.id, "u_inv_texture_size");
    p.loc_time = rlGetLocationUniform(p.id, "u_time");
    p.loc_texture0 = rlGetLocationUniform(p.id, "texture0");
    p.loc_source_size = rlGetLocationUniform(p.id, "u_source_size");
    p.loc_scale = rlGetLocationUniform(p.id, "u_scale");
    p.attr_position = rlGetLocationAttrib(p.id, "vertexPosition");
    p.attr_texcoord = rlGetLocationAttrib(p.id, "vertexTexCoord");
    p.attr_color = rlGetLocationAttrib(p.id, "vertexColor");
}

bool GpuBatcher::init(bool rlgl_batch) {
    release();
    rlgl_batch_ = rlgl_batch;
    const std::string vs = build_or_empty(ShaderStage::Vertex, engine_vertex_shader_body());
    const std::string fs = build_or_empty(ShaderStage::Fragment, engine_fragment_shader_body());
    const std::string ups = build_or_empty(ShaderStage::Fragment, upscale_fragment_shader_body());
    engine_.id = compile(vs.c_str(), fs.c_str());
    upscale_.id = compile(vs.c_str(), ups.c_str());
    if (engine_.id == 0 || upscale_.id == 0) {
        LAM_LOG_ERROR("render: the engine shaders did not compile; nothing will be drawn");
        release();
        return false;
    }
    engine_.program.reset(engine_.id);
    upscale_.program.reset(upscale_.id);
    setup_program(engine_);
    setup_program(upscale_);

    // Quad q: (4q, 4q+1, 4q+2) and (4q, 4q+2, 4q+3).
    std::vector<std::uint16_t> indices(static_cast<std::size_t>(kSegmentQuads) * 6);
    for (std::uint32_t q = 0; q < kSegmentQuads; ++q) {
        const auto base = static_cast<std::uint16_t>(q * 4);
        std::uint16_t *i = &indices[static_cast<std::size_t>(q) * 6];
        i[0] = base;
        i[1] = static_cast<std::uint16_t>(base + 1);
        i[2] = static_cast<std::uint16_t>(base + 2);
        i[3] = base;
        i[4] = static_cast<std::uint16_t>(base + 2);
        i[5] = static_cast<std::uint16_t>(base + 3);
    }
    vao_.reset(rlLoadVertexArray());
    if (!vao_.valid()) {
        LAM_LOG_WARN("render: no vertex array objects; using rlgl's batch (fallback)");
        rlgl_batch_ = true;
    } else {
        rlEnableVertexArray(vao_.get());
        ibo_.reset(rlLoadVertexBufferElement(
            indices.data(), static_cast<int>(indices.size() * sizeof(std::uint16_t)), false));
        quad_vbo_.reset(rlLoadVertexBuffer(nullptr, 4 * kStride, true));
        rlDisableVertexArray();
        rlDisableVertexBuffer();
    }
    for (auto &frame : ring_)
        frame.reserve(8);
    placements_.reserve(256);
    targets_.resize(1 + RenderPasses::kMaxTargets);
    cleared_view_.assign(targets_.size(), 0);
    if (rlgl_batch_) {
        fallback_batch_ = rlLoadRenderBatch(1, static_cast<int>(kSegmentQuads));
        fallback_batch_loaded_ = true;
        LAM_LOG_INFO("render: drawing through rlgl's batch (LAM_RENDER_RLGL_BATCH fallback)");
    }
    ready_ = true;
    return true;
}

void GpuBatcher::release() {
    if (fallback_batch_loaded_) {
        rlUnloadRenderBatch(fallback_batch_);
        fallback_batch_ = {};
        fallback_batch_loaded_ = false;
    }
    for (auto &frame : ring_)
        frame.clear();
    targets_.clear();
    quad_vbo_.reset();
    ibo_.reset();
    vao_.reset();
    custom_.clear();
    upscale_.program.reset();
    engine_.program.reset();
    engine_ = GpuProgram{};
    upscale_ = GpuProgram{};
    ready_ = false;
}

bool GpuBatcher::ensure_target(Target &t, IVec2 size, TextureFilter filter) {
    size = {std::max(size.x, 1), std::max(size.y, 1)};
    if (t.rt.valid() && t.size == size && t.filter == filter) return true;
    t.rt.reset(LoadRenderTexture(size.x, size.y));
    t.size = size;
    t.filter = filter;
    if (!t.rt.valid()) return false;
    rlTextureParameters(t.rt.get().texture.id, RL_TEXTURE_MIN_FILTER, gl_filter(filter));
    rlTextureParameters(t.rt.get().texture.id, RL_TEXTURE_MAG_FILTER, gl_filter(filter));
    rlTextureParameters(t.rt.get().texture.id, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
    rlTextureParameters(t.rt.get().texture.id, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
    return true;
}

void GpuBatcher::use_program(const GpuProgram &p) {
    if (cur_program_ == p.id) return;
    rlEnableShader(p.id);
    cur_program_ = p.id;
    cur_view_ = -1; // uniforms are per program
}

void GpuBatcher::bind_vao() {
    if (vao_bound_ || !vao_.valid()) return;
    rlEnableVertexArray(vao_.get());
    vao_bound_ = true;
}

void GpuBatcher::apply_blend(BlendMode mode) {
    const int m = static_cast<int>(mode);
    if (cur_blend_ == m) return;
    cur_blend_ = m;
    // rlSetBlendMode flushes rlgl's batch, which unbinds our VAO and program. Rebind both.
    cur_program_ = 0;
    vao_bound_ = false;
    switch (mode) {
    case BlendMode::Alpha:
        rlSetBlendMode(RL_BLEND_ALPHA);
        break;
    case BlendMode::Additive:
        rlSetBlendMode(RL_BLEND_ADDITIVE);
        break;
    case BlendMode::Multiply:
        rlSetBlendFactors(RL_DST_COLOR, RL_ZERO, RL_FUNC_ADD);
        rlSetBlendMode(RL_BLEND_CUSTOM);
        break;
    case BlendMode::Premultiplied:
        rlSetBlendMode(RL_BLEND_ALPHA_PREMULTIPLY);
        break;
    case BlendMode::Opaque:
        rlSetBlendFactors(RL_ONE, RL_ZERO, RL_FUNC_ADD);
        rlSetBlendMode(RL_BLEND_CUSTOM);
        break;
    }
}

void GpuBatcher::bind_target(std::size_t index, const SubmitFrame &f) {
    if (cur_target_ == index) return;
    if (rlgl_batch_) rlDrawRenderBatchActive();
    cur_target_ = index;
    Target &t = targets_[index];
    rlEnableFramebuffer(t.rt.get().id);
    rlViewport(0, 0, t.size.x, t.size.y);
    cur_clip_ = -1;
    rlDisableScissorTest();
    (void)f;
}

void GpuBatcher::apply_clip(const SubmitFrame &f, std::uint8_t clip, IVec2 target_size) {
    if (cur_clip_ == clip) return;
    if (rlgl_batch_) rlDrawRenderBatchActive();
    cur_clip_ = clip;
    if (clip == 0 || clip > f.clips.size()) {
        rlDisableScissorTest();
        return;
    }
    const IRect r = f.clips[clip - 1u];
    const float sx = static_cast<float>(target_size.x) /
                     static_cast<float>(std::max(f.layout.virtual_size.x, 1));
    const float sy = static_cast<float>(target_size.y) /
                     static_cast<float>(std::max(f.layout.virtual_size.y, 1));
    const int x = static_cast<int>(std::floor(static_cast<float>(r.x) * sx));
    const int w = static_cast<int>(std::ceil(static_cast<float>(r.w) * sx));
    const int h = static_cast<int>(std::ceil(static_cast<float>(r.h) * sy));
    // Targets are drawn y-down: virtual row 0 is the top, GL's scissor origin the bottom.
    const int y = target_size.y - static_cast<int>(std::floor(static_cast<float>(r.y) * sy)) - h;
    rlEnableScissorTest();
    rlScissor(x, y, std::max(w, 0), std::max(h, 0));
}

const GpuProgram &GpuBatcher::program_for(const SubmitFrame &f, MaterialId material) {
    if (f.materials == nullptr || f.assets == nullptr) return engine_;
    const Material &m = f.materials->get(material < f.materials->size() ? material : 0);
    if (!m.shader) return engine_;
    const Shader *s = f.assets->get<Shader>(m.shader);
    if (s == nullptr || s->gpu_id == 0) return engine_;
    const std::uint64_t bits = m.shader.bits();
    const std::uint32_t version = f.assets->version(m.shader);
    GpuProgram *reuse = nullptr;
    for (GpuProgram &p : custom_) {
        if (p.id != s->gpu_id) continue;
        if (p.shader_bits == bits && p.shader_version == version) return p;
        reuse = &p; // GL reused the id for a reloaded shader; locations are stale
    }
    GpuProgram &p = reuse != nullptr ? *reuse : custom_.emplace_back();
    p.id = s->gpu_id;
    p.shader_bits = bits;
    p.shader_version = version;
    setup_program(p);
    return p;
}

void GpuBatcher::set_view_uniforms(const GpuProgram &p, const SubmitFrame &f, const Batch &b) {
    // -3: composite (identity, no snap), -2: screen layers (identity), else the view's matrix.
    const int key = b.composite_source != 0 ? -3 : (b.screen_space ? -2 : static_cast<int>(b.view));
    if (cur_view_ == key) return;
    cur_view_ = key;
    const bool known = b.view < f.views.size();
    if (key >= 0 && known) {
        const Mat2x3 &m = f.views[b.view].world_to_screen;
        set_vec4(p.loc_view_x, m.a, m.c, m.tx, 0.0f);
        set_vec4(p.loc_view_y, m.b, m.d, m.ty, 0.0f);
    } else {
        set_vec4(p.loc_view_x, 1.0f, 0.0f, 0.0f, 0.0f);
        set_vec4(p.loc_view_y, 0.0f, 1.0f, 0.0f, 0.0f);
    }
    const bool snap = key != -3 && (!known || f.views[b.view].pixel_snap);
    set_float(p.loc_snap, snap ? 1.0f : 0.0f);
    set_float(p.loc_time, f.time_seconds);
    if (p.loc_mvp >= 0) {
        rlSetUniformMatrix(p.loc_mvp, ortho_y_down(static_cast<float>(f.layout.virtual_size.x),
                                                   static_cast<float>(f.layout.virtual_size.y)));
    }
}

void GpuBatcher::point_attributes(const GpuProgram &p, unsigned int vbo, std::size_t first_vertex) {
    rlEnableVertexBuffer(vbo);
    const int base = static_cast<int>(first_vertex) * kStride;
    if (p.attr_position >= 0) {
        rlSetVertexAttribute(static_cast<unsigned>(p.attr_position), 2, RL_FLOAT, false, kStride,
                             base);
        rlEnableVertexAttribute(static_cast<unsigned>(p.attr_position));
    }
    if (p.attr_texcoord >= 0) {
        rlSetVertexAttribute(static_cast<unsigned>(p.attr_texcoord), 2, RL_FLOAT, false, kStride,
                             base + 8);
        rlEnableVertexAttribute(static_cast<unsigned>(p.attr_texcoord));
    }
    if (p.attr_color >= 0) {
        rlSetVertexAttribute(static_cast<unsigned>(p.attr_color), 4, RL_UNSIGNED_BYTE, true,
                             kStride, base + 16);
        rlEnableVertexAttribute(static_cast<unsigned>(p.attr_color));
    }
}

void GpuBatcher::draw_quad_immediate(const GpuProgram &p, unsigned int texture,
                                     const QuadVertex (&v)[4], IVec2 tex_size) {
    bind_vao();
    rlUpdateVertexBuffer(quad_vbo_.get(), v, 4 * kStride, 0);
    set_vec2(p.loc_inv_size, 1.0f / static_cast<float>(std::max(tex_size.x, 1)),
             1.0f / static_cast<float>(std::max(tex_size.y, 1)));
    rlActiveTextureSlot(0);
    rlEnableTexture(texture);
    point_attributes(p, quad_vbo_.get(), 0);
    rlDrawVertexArrayElements(0, 6, nullptr);
    ++drawn_;
}

std::uint32_t GpuBatcher::submit(const SubmitFrame &f, std::span<GpuTexture> textures,
                                 std::uint32_t *skipped_unloaded, ImageData *capture) {
    if (!ready_) return 0;
    drawn_ = 0;
    cur_program_ = 0;
    cur_blend_ = -1;
    cur_target_ = static_cast<std::size_t>(-1);
    cur_clip_ = -1;
    cur_view_ = -1;
    vao_bound_ = false;
    ++frame_;

    // Virtual screen plus pass targets; only (re)created on warm-up and resize.
    const IVec2 vsize = f.layout.virtual_size;
    bool targets_ok = true;
    {
        DriverAllocScope gl(driver_allocs_);
        rlDrawRenderBatchActive();
        rlDisableBackfaceCulling();
        rlDisableDepthTest();
        targets_ok = ensure_target(targets_[0], vsize, TextureFilter::Bilinear);
        if (f.passes != nullptr) {
            for (std::size_t i = 1; i <= f.passes->target_count() && i < targets_.size(); ++i) {
                const auto id = static_cast<RenderTargetId>(i);
                targets_ok = ensure_target(targets_[i], f.passes->target_size(id, vsize),
                                           f.passes->desc(id).filter) &&
                             targets_ok;
            }
        }
    }
    if (!targets_ok) {
        LAM_LOG_ERROR("render: cannot create render targets");
        rlEnableBackfaceCulling();
        return 0;
    }

    if (f.draw_scene) {
        std::fill(cleared_view_.begin(), cleared_view_.end(), 0u);
        {
            DriverAllocScope gl(driver_allocs_);
            bind_target(0, f);
            rlClearColor(f.clear_color.r, f.clear_color.g, f.clear_color.b, 255);
            rlClearScreenBuffers();
        }
        if (rlgl_batch_) {
            DriverAllocScope gl(driver_allocs_);
            drawn_ += submit_rlgl(f, textures, skipped_unloaded);
        } else {
            draw_plan(f, textures, skipped_unloaded);
        }
    }

    if (capture != nullptr && targets_[0].rt.valid()) {
        // GL rows are bottom-up; flip to top row first.
        const Target &vt = targets_[0];
        void *px = nullptr;
        {
            DriverAllocScope gl(driver_allocs_);
            px = rlReadTexturePixels(vt.rt.get().texture.id, vt.size.x, vt.size.y,
                                     PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        }
        if (px != nullptr) {
            capture->width = vt.size.x;
            capture->height = vt.size.y;
            const std::size_t row = static_cast<std::size_t>(vt.size.x) * 4;
            capture->rgba.resize(row * static_cast<std::size_t>(vt.size.y));
            for (int y = 0; y < vt.size.y; ++y) {
                std::memcpy(capture->rgba.data() + static_cast<std::size_t>(y) * row,
                            static_cast<const std::uint8_t *>(px) +
                                static_cast<std::size_t>(vt.size.y - 1 - y) * row,
                            row);
            }
            RL_FREE(px);
        }
    }

    DriverAllocScope gl(driver_allocs_);
    present(f);
    // Restore rlgl's state for anything raylib draws after us.
    rlDisableVertexArray();
    vao_bound_ = false;
    rlDisableVertexBuffer();
    rlDisableTexture();
    rlDisableScissorTest();
    rlDisableShader();
    rlSetBlendMode(RL_BLEND_ALPHA);
    rlEnableBackfaceCulling();
    return drawn_;
}

void GpuBatcher::draw_plan(const SubmitFrame &f, std::span<GpuTexture> textures,
                           std::uint32_t *skipped_unloaded) {
    const RenderPlan &plan = *f.plan;
    const IVec2 vsize = f.layout.virtual_size;
    // Pack batches into this frame's segments in plan order; a batch never straddles two.
    std::vector<Segment> &ring = ring_[frame_ % kFramesInFlight];
    placements_.resize(plan.batches.size()); // reserved for 256 batches
    std::uint32_t seg = 0, used = 0;
    std::size_t segments_used = 0;
    for (std::size_t i = 0; i < plan.batches.size(); ++i) {
        const Batch &b = plan.batches[i];
        if (b.quad_count == 0) continue;
        if (used + b.quad_count > kSegmentQuads) {
            ++seg;
            used = 0;
        }
        placements_[i] = Placement{seg, used};
        used += b.quad_count;
        segments_used = seg + 1;
    }
    while (ring.size() < segments_used) { // warm-up
        Segment &s = ring.emplace_back();
        DriverAllocScope gl(driver_allocs_);
        s.vbo.reset(rlLoadVertexBuffer(nullptr, kSegmentBytes, true));
    }
    {
        // One upload per segment: its batches are contiguous in the staging buffer.
        DriverAllocScope gl(driver_allocs_);
        std::size_t i = 0;
        while (i < plan.batches.size()) {
            if (plan.batches[i].quad_count == 0) {
                ++i;
                continue;
            }
            const std::uint32_t s = placements_[i].segment;
            const std::uint32_t first = plan.batches[i].first_quad;
            std::uint32_t count = 0;
            while (i < plan.batches.size() &&
                   (plan.batches[i].quad_count == 0 || placements_[i].segment == s)) {
                count += plan.batches[i].quad_count;
                ++i;
            }
            rlUpdateVertexBuffer(ring[s].vbo.get(), plan.staging.data() + std::size_t{first} * 4,
                                 static_cast<int>(count) * 4 * kStride, 0);
        }
    }
    for (std::size_t i = 0; i < plan.batches.size(); ++i) {
        const Batch &b = plan.batches[i];
        const std::size_t target = b.target < targets_.size() ? b.target : 0;
        const Material &mat =
            f.materials != nullptr
                ? f.materials->get(b.material < f.materials->size() ? b.material : 0)
                : kAlphaMaterial;
        const GpuProgram &p =
            program_for(f, b.material); // may compile a custom program on first use
        DriverAllocScope gl(driver_allocs_);
        bind_target(target, f);
        if (target != 0 && f.passes != nullptr && cleared_view_[target] != b.view + 1u) {
            cleared_view_[target] = b.view + 1u;
            const RenderTargetDesc &d = f.passes->desc(static_cast<RenderTargetId>(target));
            if (d.clear) {
                rlDisableScissorTest();
                cur_clip_ = -1;
                rlClearColor(d.clear_color.r, d.clear_color.g, d.clear_color.b, d.clear_color.a);
                rlClearScreenBuffers();
            }
        }
        apply_clip(f, b.composite_source != 0 ? 0 : b.clip, targets_[target].size);
        apply_blend(mat.blend);
        use_program(p);
        set_view_uniforms(p, f, b);
        if (b.composite_source != 0) {
            const std::size_t src = b.composite_source < targets_.size() ? b.composite_source : 0;
            if (src == 0 || src == target) continue;
            const Target &st = targets_[src];
            const float w = static_cast<float>(vsize.x), h = static_cast<float>(vsize.y);
            const float tw = static_cast<float>(st.size.x), th = static_cast<float>(st.size.y);
            // Render textures are bottom-up.
            const QuadVertex v[4] = {{{0, 0}, {0, th}, kWhite},
                                     {{w, 0}, {tw, th}, kWhite},
                                     {{w, h}, {tw, 0}, kWhite},
                                     {{0, h}, {0, 0}, kWhite}};
            draw_quad_immediate(p, st.rt.get().texture.id, v, st.size);
            continue;
        }
        const GpuTexture *t = b.texture < textures.size() ? &textures[b.texture] : nullptr;
        if (t == nullptr || !t->tex.valid()) {
            if (skipped_unloaded != nullptr) *skipped_unloaded += b.quad_count;
            continue;
        }
        bind_vao();
        set_vec2(p.loc_inv_size, 1.0f / static_cast<float>(std::max(t->width, 1)),
                 1.0f / static_cast<float>(std::max(t->height, 1)));
        rlActiveTextureSlot(0);
        rlEnableTexture(t->tex.get());
        const Placement &pl = placements_[i];
        point_attributes(p, ring[pl.segment].vbo.get(), std::size_t{pl.offset_quads} * 4);
        rlDrawVertexArrayElements(0, static_cast<int>(b.quad_count) * 6, nullptr);
        ++drawn_;
    }
    DriverAllocScope gl(driver_allocs_);
    rlDisableVertexArray();
    vao_bound_ = false;
}

void GpuBatcher::present(const SubmitFrame &f) {
    if (rlgl_batch_) rlDrawRenderBatchActive();
    rlDisableFramebuffer();
    cur_target_ = static_cast<std::size_t>(-1);
    const IVec2 fb = f.layout.framebuffer;
    rlViewport(0, 0, fb.x, fb.y);
    rlDisableScissorTest();
    rlClearColor(0, 0, 0, 255);
    rlClearScreenBuffers();
    const Target &vt = targets_[0];
    if (!vt.rt.valid()) return;
    apply_blend(BlendMode::Opaque);
    if (rlgl_batch_ || !vao_.valid()) {
        // rlgl path: plain textured quad, no sharp-bilinear shader.
        rlMatrixMode(RL_PROJECTION);
        rlLoadIdentity();
        rlOrtho(0, fb.x, fb.y, 0, -1, 1);
        rlMatrixMode(RL_MODELVIEW);
        rlLoadIdentity();
        const IRect r = f.layout.viewport;
        rlSetTexture(vt.rt.get().texture.id);
        rlBegin(RL_QUADS);
        rlColor4ub(255, 255, 255, 255);
        rlTexCoord2f(0, 1);
        rlVertex2f(static_cast<float>(r.x), static_cast<float>(r.y));
        rlTexCoord2f(0, 0);
        rlVertex2f(static_cast<float>(r.x), static_cast<float>(r.y + r.h));
        rlTexCoord2f(1, 0);
        rlVertex2f(static_cast<float>(r.x + r.w), static_cast<float>(r.y + r.h));
        rlTexCoord2f(1, 1);
        rlVertex2f(static_cast<float>(r.x + r.w), static_cast<float>(r.y));
        rlEnd();
        rlSetTexture(0);
        rlDrawRenderBatchActive();
        ++drawn_;
        return;
    }
    use_program(upscale_);
    set_vec4(upscale_.loc_view_x, 1.0f, 0.0f, 0.0f, 0.0f);
    set_vec4(upscale_.loc_view_y, 0.0f, 1.0f, 0.0f, 0.0f);
    set_float(upscale_.loc_snap, 0.0f);
    set_vec2(upscale_.loc_source_size, static_cast<float>(vt.size.x),
             static_cast<float>(vt.size.y));
    set_vec2(upscale_.loc_scale, f.layout.scale_xy.x, f.layout.scale_xy.y);
    if (upscale_.loc_mvp >= 0)
        rlSetUniformMatrix(upscale_.loc_mvp,
                           ortho_y_down(static_cast<float>(fb.x), static_cast<float>(fb.y)));
    const IRect r = f.layout.viewport;
    const float x0 = static_cast<float>(r.x), y0 = static_cast<float>(r.y);
    const float x1 = static_cast<float>(r.x + r.w), y1 = static_cast<float>(r.y + r.h);
    const float tw = static_cast<float>(vt.size.x), th = static_cast<float>(vt.size.y);
    const QuadVertex v[4] = {{{x0, y0}, {0, th}, kWhite},
                             {{x1, y0}, {tw, th}, kWhite},
                             {{x1, y1}, {tw, 0}, kWhite},
                             {{x0, y1}, {0, 0}, kWhite}};
    draw_quad_immediate(upscale_, vt.rt.get().texture.id, v, vt.size);
    rlDisableVertexArray();
    vao_bound_ = false;
}

bool GpuBatcher::screenshot(const char *path) {
    ::Image image = LoadImageFromScreen();
    if (image.data == nullptr) return false;
    const bool ok = ExportImage(image, path);
    UnloadImage(image);
    return ok;
}

// Fallback through rlgl's own batch, transforming vertices on the CPU.
std::uint32_t GpuBatcher::submit_rlgl(const SubmitFrame &f, std::span<GpuTexture> textures,
                                      std::uint32_t *skipped_unloaded) {
    const RenderPlan &plan = *f.plan;
    std::uint32_t draws = 0;
    rlSetRenderBatchActive(&fallback_batch_);
    rlMatrixMode(RL_PROJECTION);
    rlLoadIdentity();
    rlOrtho(0, f.layout.virtual_size.x, f.layout.virtual_size.y, 0, -1, 1);
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    int last_texture = -1;
    for (const Batch &b : plan.batches) {
        const std::size_t target = b.target < targets_.size() ? b.target : 0;
        bind_target(target, f);
        if (target != 0 && f.passes != nullptr && cleared_view_[target] != b.view + 1u) {
            cleared_view_[target] = b.view + 1u;
            const RenderTargetDesc &d = f.passes->desc(static_cast<RenderTargetId>(target));
            if (d.clear) {
                rlDrawRenderBatchActive();
                rlClearColor(d.clear_color.r, d.clear_color.g, d.clear_color.b, d.clear_color.a);
                rlClearScreenBuffers();
            }
        }
        apply_clip(f, b.composite_source != 0 ? 0 : b.clip, targets_[target].size);
        const Material &mat =
            f.materials != nullptr
                ? f.materials->get(b.material < f.materials->size() ? b.material : 0)
                : kAlphaMaterial;
        if (cur_blend_ != static_cast<int>(mat.blend)) rlDrawRenderBatchActive();
        apply_blend(mat.blend);
        if (b.composite_source != 0) {
            const std::size_t src = b.composite_source < targets_.size() ? b.composite_source : 0;
            if (src == 0 || src == target) continue;
            const float w = static_cast<float>(f.layout.virtual_size.x);
            const float h = static_cast<float>(f.layout.virtual_size.y);
            rlSetTexture(targets_[src].rt.get().texture.id);
            rlBegin(RL_QUADS);
            rlColor4ub(255, 255, 255, 255);
            rlTexCoord2f(0, 1);
            rlVertex2f(0, 0);
            rlTexCoord2f(0, 0);
            rlVertex2f(0, h);
            rlTexCoord2f(1, 0);
            rlVertex2f(w, h);
            rlTexCoord2f(1, 1);
            rlVertex2f(w, 0);
            rlEnd();
            rlDrawRenderBatchActive();
            last_texture = -1;
            ++draws;
            continue;
        }
        const GpuTexture *t = b.texture < textures.size() ? &textures[b.texture] : nullptr;
        if (t == nullptr || !t->tex.valid()) {
            if (skipped_unloaded != nullptr) *skipped_unloaded += b.quad_count;
            continue;
        }
        const bool world = !b.screen_space && b.view < f.views.size();
        const Mat2x3 m = world ? f.views[b.view].world_to_screen : Mat2x3{};
        const bool snap = b.view < f.views.size() ? f.views[b.view].pixel_snap : true;
        const float iw = 1.0f / static_cast<float>(std::max(t->width, 1));
        const float ih = 1.0f / static_cast<float>(std::max(t->height, 1));
        if (last_texture != static_cast<int>(b.texture)) ++draws;
        last_texture = static_cast<int>(b.texture);
        rlSetTexture(t->tex.get());
        const QuadVertex *v = plan.staging.data() + std::size_t{b.first_quad} * 4;
        for (std::uint32_t q = 0; q < b.quad_count; ++q, v += 4) {
            rlCheckRenderBatchLimit(4); // flushes when full
            rlBegin(RL_QUADS);
            for (int k = 0; k < 4; ++k) {
                Vec2 p = m.apply(v[k].pos);
                if (snap) p = {std::floor(p.x + 0.5f), std::floor(p.y + 0.5f)};
                rlColor4ub(v[k].color.r, v[k].color.g, v[k].color.b, v[k].color.a);
                rlTexCoord2f(v[k].uv.x * iw, v[k].uv.y * ih);
                rlVertex2f(p.x, p.y);
            }
            rlEnd();
        }
    }
    rlDrawRenderBatchActive();
    rlSetTexture(0);
    rlSetRenderBatchActive(nullptr);
    return draws;
}

} // namespace lam::detail
