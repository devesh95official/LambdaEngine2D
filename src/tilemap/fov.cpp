// Symmetric shadowcasting (Albert Ford, 2021) with exact integer slopes, plus explored memory.
// Rows are scanned outward per quadrant; only a floor-to-wall split recurses, so depth <= radius.
#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include <lam/tilemap/fov.h>

#include "detail/grid_codec.h"

namespace lam {

namespace {

// A slope num / den (den > 0): the column offset per row of depth.
struct Slope {
    std::int64_t num;
    std::int64_t den;
};

std::int64_t floor_div(std::int64_t a, std::int64_t b) { // b > 0
    const std::int64_t q = a / b;
    return (a % b != 0 && a < 0) ? q - 1 : q;
}

std::int64_t ceil_div(std::int64_t a, std::int64_t b) { // b > 0
    return -floor_div(-a, b);
}

struct Scan {
    IVec2 origin;
    int quadrant = 0; // 0 north, 1 east, 2 south, 3 west
    std::int64_t r2 = 0;
    int max_depth = 0;
    OpaqueFn opaque = nullptr;
    const void *ctx = nullptr;
    Grid<std::uint8_t> *visible = nullptr;

    IVec2 cell(int depth, int col) const {
        switch (quadrant) {
        case 0:
            return {origin.x + col, origin.y - depth};
        case 1:
            return {origin.x + depth, origin.y + col};
        case 2:
            return {origin.x + col, origin.y + depth};
        default:
            return {origin.x - depth, origin.y + col};
        }
    }
};

// The slope through the left edge of (depth, col): (2 col - 1) / (2 depth).
Slope edge_slope(int depth, int col) {
    return {2 * std::int64_t(col) - 1, 2 * std::int64_t(depth)};
}

void scan(const Scan &s, int depth, Slope start, Slope end) {
    while (depth <= s.max_depth) {
        // round_ties_up(depth * start) .. round_ties_down(depth * end)
        const std::int64_t min_col = floor_div(2 * depth * start.num + start.den, 2 * start.den);
        const std::int64_t max_col = ceil_div(2 * depth * end.num - end.den, 2 * end.den);
        bool have_prev = false;
        bool prev_wall = false;
        for (std::int64_t col64 = min_col; col64 <= max_col; ++col64) {
            const int col = static_cast<int>(col64);
            const IVec2 c = s.cell(depth, col);
            const bool wall = s.opaque(s.ctx, c);
            // Symmetric: the cell's centre lies inside the row's slope range.
            const bool symmetric = col64 * start.den >= std::int64_t(depth) * start.num &&
                                   col64 * end.den <= std::int64_t(depth) * end.num;
            if ((wall || symmetric) && col64 * col64 + std::int64_t(depth) * depth <= s.r2) {
                s.visible->set(c.x, c.y, 1);
            }
            if (have_prev && prev_wall && !wall) start = edge_slope(depth, col);
            if (have_prev && !prev_wall && wall) scan(s, depth + 1, start, edge_slope(depth, col));
            have_prev = true;
            prev_wall = wall;
        }
        if (!have_prev || prev_wall) return;
        ++depth;
    }
}

struct WorldOpacity {
    const TileWorld *world;
    const Grid<std::uint8_t> *extra;
};

bool world_opaque(const void *ctx, IVec2 cell) {
    const auto *o = static_cast<const WorldOpacity *>(ctx);
    return o->world->opaque(cell) || (o->extra != nullptr && o->extra->get_or(cell, 0) != 0);
}

} // namespace

void compute_fov(IVec2 origin, int radius, OpaqueFn opaque, const void *ctx,
                 Grid<std::uint8_t> &visible) {
    visible.fill(0);
    if (radius < 0 || opaque == nullptr) return;
    visible.set(origin.x, origin.y, 1);
    // Nothing beyond the grid can be marked, so deeper rows are pointless (and bounded recursion).
    const int reach = std::max(visible.width(), visible.height()) +
                      std::max(std::abs(origin.x), std::abs(origin.y));
    Scan s;
    s.origin = origin;
    s.r2 = std::int64_t(radius) * (std::int64_t(radius) + 1);
    s.max_depth = std::min(radius, reach);
    s.opaque = opaque;
    s.ctx = ctx;
    s.visible = &visible;
    for (int q = 0; q < 4; ++q) {
        s.quadrant = q;
        scan(s, 1, Slope{-1, 1}, Slope{1, 1});
    }
}

void update_fov(FovMemory &m, const TileWorld &w, IVec2 origin, int radius,
                const Grid<std::uint8_t> *extra_opaque) {
    const int width = w.shapes.width();
    const int height = w.shapes.height();
    if (m.visible.width() != width || m.visible.height() != height)
        m.visible.resize(width, height, 0);
    if (m.explored.width() != width || m.explored.height() != height)
        m.explored.resize(width, height, 0);
    const WorldOpacity o{&w, extra_opaque};
    compute_fov(origin, radius, &world_opaque, &o, m.visible);
    const auto seen = m.visible.cells();
    const auto known = m.explored.cells();
    for (std::size_t i = 0; i < seen.size(); ++i)
        known[i] = static_cast<std::uint8_t>(known[i] | (seen[i] != 0));
}

Status lam_read(FovMemory &m, DataReader &in, const SerializeContext &) {
    detail::read_grid(in.object("visible"), m.visible);
    detail::read_grid(in.object("explored"), m.explored);
    if (in.failed())
        return make_error(ErrorCode::Parse, "FovMemory: " + in.path() + ": invalid fog grids");
    if (m.visible.width() != m.explored.width() || m.visible.height() != m.explored.height()) {
        in.fail("visible and explored differ in size");
        return make_error(ErrorCode::Invalid, "FovMemory: visible and explored differ in size");
    }
    return {};
}

void lam_write(const FovMemory &m, DataWriter &out, const SerializeContext &) {
    out.begin_object();
    out.key("visible");
    detail::write_grid(m.visible, out);
    out.key("explored");
    detail::write_grid(m.explored, out);
    out.end_object();
}

void lam_hash(const FovMemory &m, Hasher &h) {
    detail::hash_grid(m.visible, h);
    detail::hash_grid(m.explored, h);
}

} // namespace lam
