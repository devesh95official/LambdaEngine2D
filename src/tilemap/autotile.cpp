// Autotiling. Only sizing the output grid allocates.
#include <algorithm>
#include <array>
#include <cstdint>

#include <lam/tilemap/autotile.h>

namespace lam {

namespace {

// Rank of each reduced blob mask (-1 for masks that are not reduced), built once at compile time.
constexpr std::array<std::int8_t, 256> make_blob_ranks() {
    std::array<std::int8_t, 256> r{};
    int rank = 0;
    for (int m = 0; m < 256; ++m) {
        const bool n = (m & kBlobN) != 0, e = (m & kBlobE) != 0, s = (m & kBlobS) != 0,
                   w = (m & kBlobW) != 0;
        const bool ok = (!(m & kBlobNE) || (n && e)) && (!(m & kBlobSE) || (s && e)) &&
                        (!(m & kBlobSW) || (s && w)) && (!(m & kBlobNW) || (n && w));
        r[static_cast<std::size_t>(m)] = ok ? static_cast<std::int8_t>(rank++) : std::int8_t{-1};
    }
    return r;
}
constexpr std::array<std::int8_t, 256> kBlobRanks = make_blob_ranks();
static_assert(kBlobRanks[255] == 46, "47 reduced blob masks");

const AutotileRule *rule_for(std::span<const AutotileRule> rules, std::uint8_t value) {
    if (value == 0) return nullptr;
    for (const AutotileRule &r : rules) {
        if (r.terrain_value == value) return &r;
    }
    return nullptr;
}

// Does the neighbour at (x, y) count as the same terrain as `rule`?
bool same_as(const Grid<std::uint8_t> &terrain, int x, int y, std::uint8_t value, bool outside_same,
             std::span<const std::uint8_t> blends) {
    if (!terrain.inside(x, y)) return outside_same;
    const std::uint8_t v = terrain(x, y);
    if (v == value) return true;
    return std::find(blends.begin(), blends.end(), v) != blends.end();
}

int edge_mask_of(const Grid<std::uint8_t> &t, IVec2 p, std::uint8_t value, bool outside,
                 std::span<const std::uint8_t> blends) {
    int mask = 0;
    if (!same_as(t, p.x, p.y - 1, value, outside, blends)) mask |= kEdgeNorth;
    if (!same_as(t, p.x + 1, p.y, value, outside, blends)) mask |= kEdgeEast;
    if (!same_as(t, p.x, p.y + 1, value, outside, blends)) mask |= kEdgeSouth;
    if (!same_as(t, p.x - 1, p.y, value, outside, blends)) mask |= kEdgeWest;
    return mask;
}

int blob_mask_of(const Grid<std::uint8_t> &t, IVec2 p, std::uint8_t value, bool outside,
                 std::span<const std::uint8_t> blends) {
    const auto same = [&](int dx, int dy) {
        return same_as(t, p.x + dx, p.y + dy, value, outside, blends);
    };
    const bool n = same(0, -1), e = same(1, 0), s = same(0, 1), w = same(-1, 0);
    int mask = (n ? kBlobN : 0) | (e ? kBlobE : 0) | (s ? kBlobS : 0) | (w ? kBlobW : 0);
    if (n && e && same(1, -1)) mask |= kBlobNE;
    if (s && e && same(1, 1)) mask |= kBlobSE;
    if (s && w && same(-1, 1)) mask |= kBlobSW;
    if (n && w && same(-1, -1)) mask |= kBlobNW;
    return mask;
}

TileId resolve(const Grid<std::uint8_t> &terrain, std::span<const AutotileRule> rules,
               std::uint32_t seed, int x, int y) {
    const AutotileRule *rule = rule_for(rules, terrain(x, y));
    if (rule == nullptr) return 0;
    const std::uint32_t variants = std::max<std::uint32_t>(rule->variants, 1u);
    const std::uint32_t v = variants > 1 ? tile_hash(x, y, seed) % variants : 0u;
    std::uint32_t mask = 0;
    switch (rule->mode) {
    case AutotileMode::Edge4:
        mask = static_cast<std::uint32_t>(
            edge_mask_of(terrain, {x, y}, rule->terrain_value, rule->edge_outside, rule->blends));
        break;
    case AutotileMode::Blob47:
        mask = static_cast<std::uint32_t>(kBlobRanks[static_cast<std::size_t>(
            blob_mask_of(terrain, {x, y}, rule->terrain_value, rule->edge_outside, rule->blends))]);
        break;
    case AutotileMode::Plain:
        break;
    }
    return rule->first + mask * variants + v;
}

} // namespace

int edge_mask4(const Grid<std::uint8_t> &terrain, IVec2 p, std::uint8_t same, bool outside_same) {
    return edge_mask_of(terrain, p, same, outside_same, {});
}

int blob_mask47(const Grid<std::uint8_t> &terrain, IVec2 p, std::uint8_t same, bool outside_same) {
    return blob_mask_of(terrain, p, same, outside_same, {});
}

int blob47_index(int mask) {
    if (mask < 0 || mask > 255) return -1;
    return kBlobRanks[static_cast<std::size_t>(mask)];
}

std::uint32_t tile_hash(int x, int y, std::uint32_t seed) {
    std::uint32_t n = static_cast<std::uint32_t>(x) * 374761393u +
                      static_cast<std::uint32_t>(y) * 668265263u + seed * 2246822519u + 0x9E3779B9u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return n ^ (n >> 16);
}

void autotile(const Grid<std::uint8_t> &terrain, std::span<const AutotileRule> rules,
              std::uint32_t seed, Grid<TileId> &out) {
    if (out.width() != terrain.width() || out.height() != terrain.height()) {
        out.resize(terrain.width(), terrain.height(), 0);
    }
    for (int y = 0; y < terrain.height(); ++y) {
        for (int x = 0; x < terrain.width(); ++x)
            out(x, y) = resolve(terrain, rules, seed, x, y);
    }
}

void autotile_cell(const Grid<std::uint8_t> &terrain, std::span<const AutotileRule> rules,
                   std::uint32_t seed, IVec2 cell, Grid<TileId> &out) {
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int x = cell.x + dx;
            const int y = cell.y + dy;
            if (!terrain.inside(x, y) || !out.inside(x, y)) continue;
            out(x, y) = resolve(terrain, rules, seed, x, y);
        }
    }
}

void autotile_details(const Grid<std::uint8_t> &terrain, std::span<const AutotileRule> rules,
                      std::uint32_t seed, Grid<TileId> &out, const Grid<std::uint8_t> *blocked) {
    if (out.width() != terrain.width() || out.height() != terrain.height()) {
        out.resize(terrain.width(), terrain.height(), 0);
    }
    for (int y = 0; y < terrain.height(); ++y) {
        for (int x = 0; x < terrain.width(); ++x) {
            out(x, y) = 0;
            const AutotileRule *rule = rule_for(rules, terrain(x, y));
            if (rule == nullptr || rule->details.empty() || rule->detail_percent == 0) continue;
            if (tile_hash(x, y, seed ^ 0xA5A5A5A5u) % 100u >= rule->detail_percent) continue;
            if (blocked != nullptr && blocked->get_or(x, y, 0) != 0) continue;
            out(x, y) = rule->details[tile_hash(x, y, seed ^ 0x3C6EF372u) % rule->details.size()];
        }
    }
}

} // namespace lam
