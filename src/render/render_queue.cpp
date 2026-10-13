// RenderQueue push API. Quads are always wound tl, tr, br, bl; flips swap UVs instead of moving
// vertices so the winding never changes.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <lam/core/assert.h>
#include <lam/core/log.h>
#include <lam/math/scalar.h>
#include <lam/render/render_queue.h>

namespace lam {
namespace {

constexpr std::size_t kMaxClips = 255;

struct Uv {
    float left, top, right, bottom;
};

Uv uv_of(const Rect &source) {
    return {source.x, source.y, source.x + source.w, source.y + source.h};
}

// Inset half a texel so edges don't sample the neighbouring cell. Without this every Light2D
// had a bright column down its left edge.
Uv shape_uv_of(const Rect &cell) {
    return {cell.x + 0.5f, cell.y + 0.5f, cell.x + cell.w - 0.5f, cell.y + cell.h - 0.5f};
}

void axis_quad(const Rect &r, const Uv &uv, Color c, QuadVertex (&v)[4]) {
    const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    v[0] = {{x0, y0}, {uv.left, uv.top}, c};
    v[1] = {{x1, y0}, {uv.right, uv.top}, c};
    v[2] = {{x1, y1}, {uv.right, uv.bottom}, c};
    v[3] = {{x0, y1}, {uv.left, uv.bottom}, c};
}

// Scales and rotates about the pivot. Flips mirror about the pivot by swapping UVs.
void region_quad(const TextureRegion &r, Vec2 position, Vec2 scale, float rotation, Vec2 pivot,
                 Color tint, bool flip_x, bool flip_y, QuadVertex (&v)[4]) {
    if (scale.x < 0.0f) {
        flip_x = !flip_x;
        scale.x = -scale.x;
    }
    if (scale.y < 0.0f) {
        flip_y = !flip_y;
        scale.y = -scale.y;
    }
    const Vec2 frame{r.size.x > 0.0f ? r.size.x : r.source.w,
                     r.size.y > 0.0f ? r.size.y : r.source.h};
    float x0 = r.trim_offset.x - pivot.x * frame.x;
    float y0 = r.trim_offset.y - pivot.y * frame.y;
    float x1 = x0 + r.source.w;
    float y1 = y0 + r.source.h;
    Uv uv = uv_of(r.source);
    if (flip_x) {
        const float a = -x1, b = -x0;
        x0 = a;
        x1 = b;
        std::swap(uv.left, uv.right);
    }
    if (flip_y) {
        const float a = -y1, b = -y0;
        y0 = a;
        y1 = b;
        std::swap(uv.top, uv.bottom);
    }
    x0 *= scale.x;
    x1 *= scale.x;
    y0 *= scale.y;
    y1 *= scale.y;
    const Vec2 local[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    const Vec2 tex[4] = {
        {uv.left, uv.top}, {uv.right, uv.top}, {uv.right, uv.bottom}, {uv.left, uv.bottom}};
    if (rotation != 0.0f) {
        const float c = det_cos(rotation), s = det_sin(rotation);
        for (int i = 0; i < 4; ++i) {
            const Vec2 p = local[i];
            v[i] = {{position.x + p.x * c - p.y * s, position.y + p.x * s + p.y * c}, tex[i], tint};
        }
    } else {
        for (int i = 0; i < 4; ++i)
            v[i] = {position + local[i], tex[i], tint};
    }
}

} // namespace

RenderQueue::RenderQueue(std::size_t quad_capacity) {
    quads_.reserve(quad_capacity);
    views_.reserve(kMaxViews);
    clips_.reserve(kMaxClips);
}

std::uint8_t RenderQueue::begin_view(const CameraView &view) {
    LAM_ASSERT(views_.size() < kMaxViews,
               "RenderQueue::begin_view: more than kMaxViews views this frame");
    if (views_.size() >= kMaxViews) {
        view_ =
            static_cast<std::uint8_t>(kMaxViews - 1); // release builds: reuse the last view
        clip_ = 0;
        return view_;
    }
    view_ = static_cast<std::uint8_t>(views_.size());
    views_.push_back(view);
    views_.back().view_id = view_;
    clip_ = 0;
    return view_;
}

void RenderQueue::set_view(std::uint8_t view) {
    LAM_ASSERT(view < kMaxViews && (views_.empty() || view < views_.size()),
               "RenderQueue::set_view: view id was not returned by begin_view this frame");
    view_ = static_cast<std::uint8_t>(view & 7u);
    clip_ = 0;
}

std::uint8_t RenderQueue::current_view() const {
    return view_;
}

std::span<const CameraView> RenderQueue::views() const {
    return views_;
}

std::uint8_t RenderQueue::clip(IRect virtual_rect) {
    // Reuse the id for a repeated rect so its quads still batch.
    for (std::size_t i = 0; i < clips_.size(); ++i) {
        if (clips_[i] == virtual_rect) return static_cast<std::uint8_t>(i + 1);
    }
    LAM_ASSERT(clips_.size() < kMaxClips, "RenderQueue::clip: more than 255 clip rects this frame");
    if (clips_.size() >= kMaxClips) return 0; // release builds: draw unclipped
    clips_.push_back(virtual_rect);
    return static_cast<std::uint8_t>(clips_.size());
}

void RenderQueue::set_clip(std::uint8_t clip) {
    LAM_ASSERT(clip <= clips_.size(), "RenderQueue::set_clip: unknown clip id");
    clip_ = clip <= clips_.size() ? clip : 0;
}

std::span<const IRect> RenderQueue::clips() const {
    return clips_;
}

void RenderQueue::set_shape_texture(AssetId<Texture> page) {
    shape_texture_ = page;
}

AssetId<Texture> RenderQueue::shape_texture() const {
    return shape_texture_;
}

void RenderQueue::push(std::uint8_t layer, float depth, MaterialId material,
                       AssetId<Texture> texture, const QuadVertex (&v)[4], std::uint32_t tiebreak) {
    LAM_ASSERT(material < kMaxMaterials, "RenderQueue::push: material id >= kMaxMaterials");
    const std::uint32_t seq = sequence_++;
    // Screen layers: push order goes in the depth bits, so later pushes draw on top.
    const std::uint32_t depth24 =
        is_screen_layer(layer) ? std::min<std::uint32_t>(seq, 0xFFFFFFu) : depth_bits(depth);
    const std::uint64_t key =
        make_sort_key(view_, layer, depth24, static_cast<MaterialId>(material & 31u),
                      texture_slot(texture), clip_);
#if LAM_DEV
    if (quads_.size() == quads_.capacity()) {
        LAM_LOG_WARN("RenderQueue: more than {} quads this frame; growing (raise "
                     "RenderConfig::quad_capacity)",
                     quads_.capacity());
    }
#endif
    DrawQuad &q = quads_.emplace_back();
    q.key = key;
    q.tiebreak = tiebreak != 0 ? tiebreak : seq;
    for (int i = 0; i < 4; ++i)
        q.v[i] = v[i];
}

void RenderQueue::sprite(const Sprite &s, const Transform2D &t, std::uint32_t tiebreak) {
    if ((s.flags & kHidden) != 0) return;
    const float depth = (s.flags & kYSort) != 0 ? t.position.y + s.sort_offset : s.depth;
    QuadVertex v[4];
    region_quad(s.region, t.position, t.scale, t.rotation, s.pivot, s.tint, (s.flags & kFlipX) != 0,
                (s.flags & kFlipY) != 0, v);
    push(s.layer, depth, s.material, s.region.texture, v, tiebreak);
}

void RenderQueue::region(const TextureRegion &r, Vec2 position, Vec2 scale, float rotation,
                         Vec2 pivot, Color tint, std::uint8_t layer, float depth,
                         MaterialId material, bool flip_x) {
    QuadVertex v[4];
    region_quad(r, position, scale, rotation, pivot, tint, flip_x, false, v);
    push(layer, depth, material, r.texture, v);
}

void RenderQueue::rect(Rect r, Color c, std::uint8_t layer, float depth) {
    if (r.empty()) return;
    QuadVertex v[4];
    axis_quad(r, uv_of(kWhiteSource), c, v);
    push(layer, depth, 0, shape_texture_, v);
}

void RenderQueue::rect_lines(Rect r, float thickness, Color c, std::uint8_t layer, float depth) {
    if (r.empty() || !(thickness > 0.0f)) return;
    // Edges don't overlap, so translucent outlines have even alpha.
    const float t = std::min({thickness, r.w * 0.5f, r.h * 0.5f});
    rect({r.x, r.y, r.w, t}, c, layer, depth);
    rect({r.x, r.y + r.h - t, r.w, t}, c, layer, depth);
    rect({r.x, r.y + t, t, r.h - 2 * t}, c, layer, depth);
    rect({r.x + r.w - t, r.y + t, t, r.h - 2 * t}, c, layer, depth);
}

void RenderQueue::line(Vec2 a, Vec2 b, float thickness, Color c, std::uint8_t layer, float depth) {
    const Vec2 d = b - a;
    const float len = length(d);
    if (!(len > 0.0f) || !(thickness > 0.0f)) return;
    const Vec2 n = perp(d / len) * (thickness * 0.5f);
    const Uv uv = uv_of(kWhiteSource);
    const QuadVertex v[4] = {{a - n, {uv.left, uv.top}, c},
                             {b - n, {uv.right, uv.top}, c},
                             {b + n, {uv.right, uv.bottom}, c},
                             {a + n, {uv.left, uv.bottom}, c}};
    push(layer, depth, 0, shape_texture_, v);
}

void RenderQueue::circle(Vec2 center, float radius, Color c, std::uint8_t layer, float depth) {
    ellipse(center, {radius, radius}, c, layer, depth);
}

void RenderQueue::ellipse(Vec2 center, Vec2 radii, Color c, std::uint8_t layer, float depth) {
    if (!(radii.x > 0.0f) || !(radii.y > 0.0f)) return;
    QuadVertex v[4];
    axis_quad(Rect::from_center(center, radii), shape_uv_of(kCircleSource), c, v);
    push(layer, depth, 0, shape_texture_, v);
}

void RenderQueue::light(Vec2 center, float radius, Color c, MaterialId material,
                        std::uint8_t layer) {
    if (!(radius > 0.0f)) return;
    QuadVertex v[4];
    axis_quad(Rect::from_center(center, {radius, radius}), shape_uv_of(kFalloffSource), c, v);
    push(layer, 0.0f, material, shape_texture_, v);
}

std::span<const DrawQuad> RenderQueue::quads() const {
    return quads_;
}

std::size_t RenderQueue::size() const {
    return quads_.size();
}

std::size_t RenderQueue::capacity() const {
    return quads_.capacity();
}

void RenderQueue::clear() {
    quads_.clear();
    views_.clear();
    clips_.clear();
    sequence_ = 0;
    view_ = 0;
    clip_ = 0;
}

} // namespace lam
