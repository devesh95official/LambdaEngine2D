#pragma once
// Render queue. Each quad has a sort key
//   view(3) | layer(8) | depth(24) | material(5) | texture(16) | clip(8)
// and a 32-bit tiebreak (entity index for y-sorted sprites, otherwise push order).
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <lam/core/asset_id.h>
#include <lam/math/color.h>
#include <lam/math/rect.h>
#include <lam/math/transform.h>
#include <lam/render/camera.h>
#include <lam/render/sprite.h>
#include <lam/render/texture.h>
namespace lam {
class RenderPasses;
inline constexpr std::uint8_t kMaxViews = 8;
inline constexpr MaterialId kMaxMaterials = 32;
inline constexpr std::uint32_t kMaxQuadsPerBatch = 16384;      // u16 index range per draw
// 1/16 px steps over +-524288 px. Done in double because float rounds odd steps up past 2^23.
constexpr std::uint32_t depth_bits(float depth) {
    const double q = (static_cast<double>(depth) + 524288.0) * 16.0;
    if (!(q > 0.0)) return 0u;
    if (q >= 16777215.0) return 0xFFFFFFu;
    return static_cast<std::uint32_t>(q + 0.5);
}
constexpr std::uint64_t make_sort_key(std::uint8_t view, std::uint8_t layer, std::uint32_t depth24,
                                      MaterialId material, std::uint16_t texture, std::uint8_t clip) {
    return (std::uint64_t(view & 7u) << 61) | (std::uint64_t(layer) << 53) |
           (std::uint64_t(depth24 & 0xFFFFFFu) << 29) | (std::uint64_t(material & 31u) << 24) |
           (std::uint64_t(texture) << 8) | std::uint64_t(clip);
}
constexpr std::uint8_t key_view(std::uint64_t k) { return static_cast<std::uint8_t>(k >> 61); }
constexpr std::uint8_t key_layer(std::uint64_t k) { return static_cast<std::uint8_t>((k >> 53) & 0xFFu); }
constexpr MaterialId key_material(std::uint64_t k) { return static_cast<MaterialId>((k >> 24) & 31u); }
constexpr std::uint16_t key_texture(std::uint64_t k) { return static_cast<std::uint16_t>((k >> 8) & 0xFFFFu); }
constexpr std::uint8_t key_clip(std::uint64_t k) { return static_cast<std::uint8_t>(k & 0xFFu); }
struct QuadVertex { Vec2 pos; Vec2 uv; Color color; };          // uv in texels
static_assert(sizeof(QuadVertex) == 20);
struct DrawQuad {
    std::uint64_t key = 0;
    std::uint32_t tiebreak = 0;
    QuadVertex v[4];                                            // tl, tr, br, bl
};
class RenderQueue {
public:
    explicit RenderQueue(std::size_t quad_capacity = 65536);
    // One view per visible world, bottom first. Returns 0..7.
    std::uint8_t begin_view(const CameraView &view);
    void set_view(std::uint8_t view);
    std::uint8_t current_view() const;
    std::span<const CameraView> views() const;
    // Virtual pixels. 0 means no clip; up to 255 per frame.
    std::uint8_t clip(IRect virtual_rect);
    void set_clip(std::uint8_t clip);
    std::span<const IRect> clips() const;                       // clips()[id - 1]
    void set_shape_texture(AssetId<Texture> page);
    AssetId<Texture> shape_texture() const;
    // tiebreak 0 uses push order.
    void push(std::uint8_t layer, float depth, MaterialId material, AssetId<Texture> texture,
              const QuadVertex (&v)[4], std::uint32_t tiebreak = 0);
    void sprite(const Sprite &s, const Transform2D &t, std::uint32_t tiebreak = 0);
    void region(const TextureRegion &r, Vec2 position, Vec2 scale, float rotation, Vec2 pivot, Color tint,
                std::uint8_t layer, float depth, MaterialId material = 0, bool flip_x = false);
    void rect(Rect r, Color c, std::uint8_t layer, float depth = 0);
    void rect_lines(Rect r, float thickness, Color c, std::uint8_t layer, float depth = 0);
    void line(Vec2 a, Vec2 b, float thickness, Color c, std::uint8_t layer, float depth = 0);
    void circle(Vec2 center, float radius, Color c, std::uint8_t layer, float depth = 0);
    void ellipse(Vec2 center, Vec2 radii, Color c, std::uint8_t layer, float depth = 0);
    // Same as draw_triangle in shapes.h.
    void triangle(Vec2 a, Vec2 b, Vec2 c, Color col, std::uint8_t layer, float depth = 0);
    void light(Vec2 center, float radius, Color c, MaterialId material, std::uint8_t layer = kLayerLights);
    std::span<const DrawQuad> quads() const;
    std::size_t size() const;
    std::size_t capacity() const;                               // can grow past this, with a dev warning
    void clear();
private:
    std::vector<DrawQuad> quads_;
    std::vector<CameraView> views_;
    std::vector<IRect> clips_;
    AssetId<Texture> shape_texture_;
    std::uint32_t sequence_ = 0;
    std::uint8_t view_ = 0;
    std::uint8_t clip_ = 0;
};
struct Batch {
    std::uint8_t view = 0;
    bool screen_space = false;
    std::uint8_t clip = 0;
    MaterialId material = 0;
    std::uint8_t target = 0;                                    // RenderTargetId; 0 = screen
    std::uint8_t composite_source = 0;                          // non-zero: draw that target instead of quads
    std::uint16_t texture = 0;
    std::uint32_t first_quad = 0;                               // into staging
    std::uint32_t quad_count = 0;
};
struct SortItem { std::uint64_t key; std::uint32_t tiebreak; std::uint32_t index; };
struct SortScratch { std::vector<SortItem> a, b; };
// Radix sorts the queue and gathers vertices into `staging`. Batches break on view, space,
// target, clip, material, texture or kMaxQuadsPerBatch, but not on layer.
void plan_batches(const RenderQueue &queue, const RenderPasses *passes, SortScratch &scratch,
                  std::vector<Batch> &batches, std::vector<QuadVertex> &staging);
// Filled by plan_batches; the backend overwrites draw_calls and the timings.
struct RenderStats {                                            // App resource
    std::uint32_t draw_calls = 0;
    std::uint32_t batches = 0;
    std::uint32_t quads = 0;
    std::uint32_t culled = 0;
    std::uint32_t skipped_unloaded = 0;
    std::uint32_t texture_switches = 0;
    std::uint32_t views = 0;
    std::uint32_t passes = 0;
    float cpu_collect_ms = 0, cpu_plan_ms = 0, cpu_upload_ms = 0, gpu_swap_ms = 0;
};
}
