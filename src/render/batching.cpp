// plan_batches: LSD radix sort on 11-bit digits, skipping digits with a single bucket, then one
// gather pass that writes staging and cuts batches. Tiebreak digits are skipped when tiebreaks
// already ascend within each layer, which is the usual case. The bench scene sorts in 4 passes.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <lam/core/assert.h>
#include <lam/render/passes.h>
#include <lam/render/render_queue.h>

namespace lam {
namespace {

constexpr int kDigitBits = 11;
constexpr std::uint32_t kBuckets = 1u << kDigitBits;
constexpr std::uint32_t kMask = kBuckets - 1;
constexpr int kTiebreakDigits = 3;           // bits 0-10, 11-21, 22-31
constexpr int kDigits = kTiebreakDigits + 6; // key bits 0-10, ..., 55-63
using Histograms = std::array<std::array<std::uint32_t, kBuckets>, kDigits>;

inline unsigned digit(const SortItem &it, int d) {
    return d < kTiebreakDigits
               ? (it.tiebreak >> (kDigitBits * d)) & kMask
               : static_cast<unsigned>((it.key >> (kDigitBits * (d - kTiebreakDigits))) & kMask);
}

// The state that forces a new batch when it changes.
struct State {
    std::uint8_t view = 0;
    bool screen = false;
    std::uint8_t target = 0;
    std::uint8_t clip = 0;
    MaterialId material = 0;
    std::uint16_t texture = 0;
    bool operator==(const State &) const = default;
};

} // namespace

void plan_batches(const RenderQueue &queue, const RenderPasses *passes, SortScratch &scratch,
                  std::vector<Batch> &batches, std::vector<QuadVertex> &staging) {
    batches.clear();
    staging.clear();
    const std::span<const DrawQuad> quads = queue.quads();
    const std::size_t n = quads.size();
    if (n == 0) return;
    LAM_CHECK(n < 0xFFFFFFFFu / 4u, "plan_batches: too many quads");

    // Layer -> render target, one byte per layer.
    std::array<std::uint8_t, 256> route{};
    if (passes != nullptr) {
        for (int l = 0; l < 256; ++l)
            route[static_cast<std::size_t>(l)] = passes->target_of(static_cast<std::uint8_t>(l));
    }

    scratch.a.resize(n);
    scratch.b.resize(n);
    SortItem *src = scratch.a.data();
    SortItem *dst = scratch.b.data();

    // One read of the queue fills every histogram, since digit counts don't depend on order.
    Histograms hist{}; // 72 KiB on the stack
    std::array<std::uint8_t, kMaxViews> used{};
    std::array<std::uint32_t, 256> last_tiebreak{};
    bool tiebreaks_ordered = true;
    for (std::size_t i = 0; i < n; ++i) {
        const DrawQuad &q = quads[i];
        src[i] = SortItem{q.key, q.tiebreak, static_cast<std::uint32_t>(i)};
        const std::uint32_t t = q.tiebreak;
        const std::uint64_t k = q.key;
        const std::uint8_t layer = key_layer(k);
        tiebreaks_ordered = tiebreaks_ordered && t >= last_tiebreak[layer];
        last_tiebreak[layer] = t;
        ++hist[0][t & kMask];
        ++hist[1][(t >> 11) & kMask];
        ++hist[2][t >> 22];
        ++hist[3][k & kMask];
        ++hist[4][(k >> 11) & kMask];
        ++hist[5][(k >> 22) & kMask];
        ++hist[6][(k >> 33) & kMask];
        ++hist[7][(k >> 44) & kMask];
        ++hist[8][k >> 55];
        used[key_view(k)] =
            static_cast<std::uint8_t>(used[key_view(k)] | (1u << (route[layer] & 7u)));
    }

    // Stable passes, so equal keys keep tiebreak order.
    std::array<std::uint32_t, kBuckets> offset;
    for (int d = tiebreaks_ordered ? kTiebreakDigits : 0; d < kDigits; ++d) {
        const std::array<std::uint32_t, kBuckets> &h = hist[static_cast<std::size_t>(d)];
        if (h[digit(src[0], d)] == n) continue; // single bucket, nothing to reorder
        std::uint32_t sum = 0;
        for (std::size_t b = 0; b < kBuckets; ++b) {
            offset[b] = sum;
            sum += h[b];
        }
        for (std::size_t i = 0; i < n; ++i)
            dst[offset[digit(src[i], d)]++] = src[i];
        std::swap(src, dst);
    }

    // Gather and cut batches.
    staging.resize(n * 4);
    QuadVertex *out = staging.data();
    const std::span<const RenderPasses::Composite> comps =
        passes != nullptr ? passes->composites() : std::span<const RenderPasses::Composite>{};
    std::size_t next_comp = 0;
    int current_view = -1;
    Batch cur;
    State cur_state;
    bool open = false;

    const auto close = [&] {
        if (open) batches.push_back(cur);
        open = false;
    };
    // layer 256 flushes all remaining composites.
    const auto emit_composites = [&](int view, int layer, std::uint32_t at_quad) {
        while (next_comp < comps.size() && static_cast<int>(comps[next_comp].layer) <= layer) {
            const RenderPasses::Composite &c = comps[next_comp++];
            const std::uint8_t dest = route[c.layer];
            if (c.source == 0 || c.source == dest) continue;
            if ((used[static_cast<std::size_t>(view)] & (1u << (c.source & 7u))) == 0) continue;
            close();
            Batch b;
            b.view = static_cast<std::uint8_t>(view);
            b.screen_space = true;
            b.clip = 0;
            b.material = c.material;
            b.target = dest;
            b.composite_source = c.source;
            b.texture = 0;
            b.first_quad = at_quad;
            b.quad_count = 0;
            batches.push_back(b);
        }
    };

    constexpr std::size_t kPrefetch = 12; // sorted order means random reads
    for (std::size_t k = 0; k < n; ++k) {
        const SortItem &it = src[k];
        if (k + kPrefetch < n) __builtin_prefetch(&quads[src[k + kPrefetch].index]);
        const DrawQuad &q = quads[it.index];
        out[0] = q.v[0];
        out[1] = q.v[1];
        out[2] = q.v[2];
        out[3] = q.v[3];
        out += 4;

        const std::uint64_t key = it.key;
        const int view = key_view(key);
        const std::uint8_t layer = key_layer(key);
        if (view != current_view) {
            if (current_view >= 0)
                emit_composites(current_view, 256, static_cast<std::uint32_t>(k));
            current_view = view;
            next_comp = 0;
        }
        if (next_comp < comps.size() && comps[next_comp].layer <= layer)
            emit_composites(view, layer, static_cast<std::uint32_t>(k));

        const State s{static_cast<std::uint8_t>(view),
                      is_screen_layer(layer),
                      route[layer],
                      key_clip(key),
                      key_material(key),
                      key_texture(key)};
        if (open && s == cur_state && cur.quad_count < kMaxQuadsPerBatch) {
            ++cur.quad_count;
            continue;
        }
        close();
        cur = Batch{};
        cur.view = s.view;
        cur.screen_space = s.screen;
        cur.clip = s.clip;
        cur.material = s.material;
        cur.target = s.target;
        cur.composite_source = 0;
        cur.texture = s.texture;
        cur.first_quad = static_cast<std::uint32_t>(k);
        cur.quad_count = 1;
        cur_state = s;
        open = true;
    }
    if (current_view >= 0) emit_composites(current_view, 256, static_cast<std::uint32_t>(n));
    close();
}

} // namespace lam
