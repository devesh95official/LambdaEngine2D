#pragma once
// raylib backend internals. GpuHandle is a move-only owner of one GPU object; everything is
// released in RaylibRenderPlugin::shutdown while the GL context is still alive.
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <raylib.h>
#include <rlgl.h>

#include <lam/assets/asset_server.h>
#include <lam/core/alloc.h>
#include <lam/core/app.h>
#include <lam/math/vec2.h>
#include <lam/render/passes.h>
#include <lam/render/render_plan.h>
#include <lam/render/render_queue.h>
#include <lam/render/shader.h>
#include <lam/render/texture.h>
#include <lam/render/virtual_screen.h>

namespace lam::detail {

template <class Deleter>
class GpuHandle {
public:
    using Resource = typename Deleter::Resource;
    GpuHandle() = default;
    explicit GpuHandle(Resource r) : r_(r) {}
    ~GpuHandle() { reset(); }
    GpuHandle(const GpuHandle &) = delete;
    GpuHandle &operator=(const GpuHandle &) = delete;
    GpuHandle(GpuHandle &&o) noexcept : r_(std::exchange(o.r_, Resource{})) {}
    GpuHandle &operator=(GpuHandle &&o) noexcept {
        if (this != &o) {
            reset();
            r_ = std::exchange(o.r_, Resource{});
        }
        return *this;
    }
    void reset() {
        if (Deleter::valid(r_)) Deleter::release(r_);
        r_ = Resource{};
    }
    void reset(Resource r) {
        reset();
        r_ = r;
    }
    // Forget without releasing, e.g. after a lost context when the id may be reused.
    void abandon() { r_ = Resource{}; }
    bool valid() const { return Deleter::valid(r_); }
    const Resource &get() const { return r_; }

private:
    Resource r_{};
};

struct TextureDeleter {
    using Resource = unsigned int;
    static bool valid(unsigned int id) { return id != 0; }
    static void release(unsigned int id) { rlUnloadTexture(id); }
};
struct RenderTextureDeleter {
    using Resource = ::RenderTexture2D;
    static bool valid(const ::RenderTexture2D &t) { return t.id != 0; }
    static void release(const ::RenderTexture2D &t) { UnloadRenderTexture(t); }
};
struct ShaderProgramDeleter {
    using Resource = unsigned int;
    static bool valid(unsigned int id) { return id != 0 && id != rlGetShaderIdDefault(); }
    static void release(unsigned int id) { rlUnloadShaderProgram(id); }
};
struct VertexBufferDeleter {
    using Resource = unsigned int;
    static bool valid(unsigned int id) { return id != 0; }
    static void release(unsigned int id) { rlUnloadVertexBuffer(id); }
};
struct VertexArrayDeleter {
    using Resource = unsigned int;
    static bool valid(unsigned int id) { return id != 0; }
    static void release(unsigned int id) { rlUnloadVertexArray(id); }
};

// Indexed by draw slot.
struct GpuTexture {
    GpuHandle<TextureDeleter> tex;
    int width = 0, height = 0;
    std::uint32_t generation = 0;    // AssetId generation uploaded
    std::uint32_t version = 0;       // Texture::version uploaded
    std::uint32_t asset_version = 0; // AssetServer::version uploaded (bumps on reload)
};

// A compiled program and the uniform locations the batcher sets (-1 = not used by it).
struct GpuProgram {
    GpuHandle<ShaderProgramDeleter> program;
    unsigned int id = 0; // program id (also for the default)
    // GL reuses program ids, so remember which shader and version the locations belong to.
    std::uint64_t shader_bits = 0;
    std::uint32_t shader_version = 0;
    int loc_mvp = -1, loc_view_x = -1, loc_view_y = -1, loc_snap = -1, loc_inv_size = -1;
    int loc_time = -1, loc_texture0 = -1, loc_source_size = -1, loc_scale = -1;
    int attr_position = -1, attr_texcoord = -1, attr_color = -1;
};

inline ShaderDialect current_shader_dialect() {
    switch (rlGetVersion()) {
    case RL_OPENGL_ES_20:
        return ShaderDialect::GlslEs100;
    case RL_OPENGL_ES_30:
        return ShaderDialect::GlslEs300;
    default:
        return ShaderDialect::Glsl330;
    }
}

struct SubmitFrame {
    const RenderPlan *plan = nullptr;
    std::span<const CameraView> views;
    std::span<const IRect> clips;
    const RenderPasses *passes = nullptr;
    const Materials *materials = nullptr;
    const AssetServer *assets = nullptr;
    VirtualLayout layout;
    Color clear_color = kBlack;
    float time_seconds = 0;
    bool draw_scene = true; // false on --draw-every skipped frames
    bool capture = false;   // ScreenCapture::request
};

// Sprite batcher: a 3-frame ring of VBO segments (16384 quads each), one shared u16 index buffer,
// and a VAO re-pointed at each batch's first vertex. `rlgl_batch` uses rlgl's own batch instead.
class GpuBatcher {
public:
    static constexpr int kFramesInFlight = 3;
    bool init(bool rlgl_batch); // false means use the fallback
    void release();
    bool rlgl_batch() const { return rlgl_batch_; }
    // Draws, presents and captures if asked. Returns the draw call count.
    std::uint32_t submit(const SubmitFrame &f, std::span<GpuTexture> textures,
                         std::uint32_t *skipped_unloaded, ImageData *capture);
    bool screenshot(const char *path);
    static unsigned int compile(const char *vs, const char *fs); // 0 on failure
    std::uint64_t driver_allocations() const { return driver_allocs_; }
    // Call when a shader asset is unloaded or reloaded.
    void forget_custom_programs() { custom_.clear(); }

private:
    struct Segment {
        GpuHandle<VertexBufferDeleter> vbo;
    };
    struct Placement {
        std::uint32_t segment = 0, offset_quads = 0;
    };
    struct Target {
        GpuHandle<RenderTextureDeleter> rt;
        IVec2 size;
        TextureFilter filter = TextureFilter::Point;
    };
    bool ensure_target(Target &t, IVec2 size, TextureFilter filter);
    void setup_program(GpuProgram &p);
    void bind_target(std::size_t index, const SubmitFrame &f);
    void use_program(const GpuProgram &p);
    void set_view_uniforms(const GpuProgram &p, const SubmitFrame &f, const Batch &b);
    void apply_blend(BlendMode mode);
    void bind_vao();
    void apply_clip(const SubmitFrame &f, std::uint8_t clip, IVec2 target_size);
    const GpuProgram &program_for(const SubmitFrame &f, MaterialId material);
    void point_attributes(const GpuProgram &p, unsigned int vbo, std::size_t first_vertex);
    void draw_quad_immediate(const GpuProgram &p, unsigned int texture, const QuadVertex (&v)[4],
                             IVec2 tex_size);
    void draw_plan(const SubmitFrame &f, std::span<GpuTexture> textures,
                   std::uint32_t *skipped_unloaded);
    std::uint32_t submit_rlgl(const SubmitFrame &f, std::span<GpuTexture> textures,
                              std::uint32_t *skipped_unloaded);
    void present(const SubmitFrame &f);

    bool ready_ = false;
    bool rlgl_batch_ = false;
    GpuProgram engine_;
    GpuProgram upscale_;
    std::vector<GpuProgram> custom_; // keyed by shader asset, version and gpu id
    GpuHandle<VertexArrayDeleter> vao_;
    GpuHandle<VertexBufferDeleter> ibo_;
    GpuHandle<VertexBufferDeleter> quad_vbo_; // composites and present (4 vertices)
    std::vector<Segment> ring_[kFramesInFlight];
    std::vector<Placement> placements_;
    std::uint64_t frame_ = 0;
    std::vector<Target> targets_;             // [0] = virtual screen, [i] = pass target i
    std::vector<std::uint32_t> cleared_view_; // per target: view + 1 last cleared
    rlRenderBatch fallback_batch_{};
    bool fallback_batch_loaded_ = false;
    // GL state cache for submit.
    unsigned int cur_program_ = 0;
    int cur_blend_ = -1;
    std::size_t cur_target_ = static_cast<std::size_t>(-1);
    int cur_clip_ = -1;
    int cur_view_ = -1;
    bool vao_bound_ = false;
    std::uint32_t drawn_ = 0;
    std::uint64_t driver_allocs_ = 0;
};

// Counts allocations made inside GL calls into `sink`. Wrap GL calls only, so the engine's own
// allocations still show up in the checks.
class DriverAllocScope {
public:
    explicit DriverAllocScope(std::uint64_t &sink) : sink_(sink), start_(allocation_count()) {}
    ~DriverAllocScope() { sink_ += allocation_count() - start_; }
    DriverAllocScope(const DriverAllocScope &) = delete;
    DriverAllocScope &operator=(const DriverAllocScope &) = delete;

private:
    std::uint64_t &sink_;
    std::uint64_t start_;
};
} // namespace lam::detail
