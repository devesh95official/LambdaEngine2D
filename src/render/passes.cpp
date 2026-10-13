// Render targets and layer routing. A per-layer byte table keeps routing branch-free.
#include <algorithm>
#include <cmath>
#include <cstddef>

#include <lam/core/assert.h>
#include <lam/math/scalar.h>
#include <lam/render/passes.h>

namespace lam {

RenderTargetId RenderPasses::add_target(StrId name, const RenderTargetDesc &d) {
    for (std::size_t i = 0; i < target_count_; ++i) {
        if (targets_[i].name == name && name) {
            targets_[i].desc = d;
            ++version_;
            return static_cast<RenderTargetId>(i + 1);
        }
    }
    LAM_CHECK(target_count_ < kMaxTargets, "RenderPasses::add_target: at most 7 targets");
    targets_[target_count_] = Target{name, d};
    ++target_count_;
    ++version_;
    return static_cast<RenderTargetId>(target_count_);
}

RenderTargetId RenderPasses::find(StrId name) const {
    if (!name) return 0;
    for (std::size_t i = 0; i < target_count_; ++i) {
        if (targets_[i].name == name) return static_cast<RenderTargetId>(i + 1);
    }
    return 0;
}

void RenderPasses::route_layers(std::uint8_t first, std::uint8_t last, RenderTargetId target) {
    LAM_ASSERT(target <= target_count_, "RenderPasses::route_layers: unknown target");
    if (target > target_count_) return;
    if (first > last) std::swap(first, last);
    for (int l = first; l <= last; ++l)
        layer_target_[static_cast<std::size_t>(l)] = target;
    ++version_;
}

void RenderPasses::composite(RenderTargetId source, MaterialId material, std::uint8_t at_layer) {
    LAM_ASSERT(source >= 1 && source <= target_count_, "RenderPasses::composite: unknown source");
    if (source < 1 || source > target_count_) return;
    LAM_CHECK(composite_count_ < kMaxComposites, "RenderPasses::composite: at most 16 composites");
    // Sorted insert; equal layers keep insertion order.
    std::size_t at = composite_count_;
    while (at > 0 && composites_[at - 1].layer > at_layer) {
        composites_[at] = composites_[at - 1];
        --at;
    }
    composites_[at] = Composite{source, material, at_layer};
    ++composite_count_;
    ++version_;
}

RenderTargetId RenderPasses::target_of(std::uint8_t layer) const {
    return layer_target_[layer];
}

std::span<const RenderPasses::Composite> RenderPasses::composites() const {
    return {composites_.data(), composite_count_};
}

std::size_t RenderPasses::target_count() const {
    return target_count_;
}

const RenderTargetDesc &RenderPasses::desc(RenderTargetId id) const {
    LAM_ASSERT(id >= 1 && id <= target_count_, "RenderPasses::desc: unknown target");
    const std::size_t i = (id >= 1 && id <= target_count_) ? id - 1u : 0u;
    return targets_[i].desc;
}

StrId RenderPasses::name_of(RenderTargetId id) const {
    if (id < 1 || id > target_count_) return {};
    return targets_[id - 1u].name;
}

IVec2 RenderPasses::target_size(RenderTargetId id, IVec2 virtual_size) const {
    const IVec2 v{std::max(virtual_size.x, 1), std::max(virtual_size.y, 1)};
    if (id < 1 || id > target_count_) return v;
    const RenderTargetDesc &d = targets_[id - 1u].desc;
    if (d.size.x > 0 && d.size.y > 0) return d.size;
    const float s = (d.virtual_scale > 0.0f && is_finite(d.virtual_scale)) ? d.virtual_scale : 1.0f;
    return {std::max(1, static_cast<int>(std::lround(static_cast<float>(v.x) * s))),
            std::max(1, static_cast<int>(std::lround(static_cast<float>(v.y) * s)))};
}

std::uint32_t RenderPasses::version() const {
    return version_;
}

} // namespace lam
