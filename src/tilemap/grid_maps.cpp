// Dijkstra maps and flow fields: multi-source Dijkstra with a decrease-key heap over preallocated
// arrays. Ties go to the lower distance, then the lower cell index.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <lam/core/assert.h>
#include <lam/tilemap/grid_maps.h>

namespace lam {

namespace {

constexpr std::uint32_t kUnreachable = std::numeric_limits<std::uint32_t>::max();

struct Step {
    int dx;
    int dy;
};
// Same fixed order as PathFinder: straight steps first.
constexpr Step kSteps[8] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};

std::uint32_t saturating_add(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t s = a + b;
    return s < a ? kUnreachable - 1u : (s == kUnreachable ? kUnreachable - 1u : s);
}

} // namespace

void DijkstraMap::resize(IVec2 size) {
    dist_.resize(size.x, size.y, kUnreachable);
    next_.resize(size.x, size.y, -1);
    heap_.assign(dist_.size(), 0);
    heap_pos_.assign(dist_.size(), -1);
    heap_size_ = 0;
}

bool DijkstraMap::heap_before(std::int32_t a, std::int32_t b) const {
    const std::uint32_t da = dist_.cells()[static_cast<std::size_t>(a)];
    const std::uint32_t db = dist_.cells()[static_cast<std::size_t>(b)];
    if (da != db) return da < db;
    return a < b;
}

void DijkstraMap::sift_up(std::int32_t pos) {
    const std::int32_t cell = heap_[static_cast<std::size_t>(pos)];
    while (pos > 0) {
        const std::int32_t parent = (pos - 1) / 2;
        const std::int32_t other = heap_[static_cast<std::size_t>(parent)];
        if (!heap_before(cell, other)) break;
        heap_[static_cast<std::size_t>(pos)] = other;
        heap_pos_[static_cast<std::size_t>(other)] = pos;
        pos = parent;
    }
    heap_[static_cast<std::size_t>(pos)] = cell;
    heap_pos_[static_cast<std::size_t>(cell)] = pos;
}

void DijkstraMap::sift_down(std::int32_t pos) {
    const std::int32_t cell = heap_[static_cast<std::size_t>(pos)];
    for (;;) {
        std::int32_t child = pos * 2 + 1;
        if (child >= heap_size_) break;
        if (child + 1 < heap_size_ && heap_before(heap_[static_cast<std::size_t>(child + 1)],
                                                  heap_[static_cast<std::size_t>(child)])) {
            ++child;
        }
        const std::int32_t other = heap_[static_cast<std::size_t>(child)];
        if (!heap_before(other, cell)) break;
        heap_[static_cast<std::size_t>(pos)] = other;
        heap_pos_[static_cast<std::size_t>(other)] = pos;
        pos = child;
    }
    heap_[static_cast<std::size_t>(pos)] = cell;
    heap_pos_[static_cast<std::size_t>(cell)] = pos;
}

void DijkstraMap::compute(const Grid<std::uint16_t> &cost, std::span<const IVec2> goals,
                          GridDirs dirs) {
    if (cost.width() != dist_.width() || cost.height() != dist_.height()) {
        LAM_ASSERT(false, "DijkstraMap::compute: the cost grid does not match resize()");
        resize({cost.width(), cost.height()}); // release builds: stay correct (allocates)
    }
    dirs_ = dirs;
    dist_.fill(kUnreachable);
    next_.fill(-1);
    for (std::int32_t &p : heap_pos_)
        p = -1;
    heap_size_ = 0;
    const int width = dist_.width();
    const int height = dist_.height();
    std::span<std::uint32_t> dist = dist_.cells();

    for (const IVec2 g : goals) {
        if (!cost.inside(g) || cost(g.x, g.y) == 0) continue;
        const std::int32_t c = g.y * width + g.x;
        if (dist[static_cast<std::size_t>(c)] == 0) continue; // duplicate goal
        dist[static_cast<std::size_t>(c)] = 0;
        heap_[static_cast<std::size_t>(heap_size_)] = c;
        heap_pos_[static_cast<std::size_t>(c)] = heap_size_;
        ++heap_size_;
        sift_up(heap_size_ - 1);
    }

    const int step_count = dirs == GridDirs::Four ? 4 : 8;
    while (heap_size_ > 0) {
        const std::int32_t cell = heap_[0];
        heap_pos_[static_cast<std::size_t>(cell)] = -2; // closed
        --heap_size_;
        if (heap_size_ > 0) {
            heap_[0] = heap_[static_cast<std::size_t>(heap_size_)];
            heap_pos_[static_cast<std::size_t>(heap_[0])] = 0;
            sift_down(0);
        }
        const int x = cell % width;
        const int y = cell / width;
        const std::uint32_t d = dist[static_cast<std::size_t>(cell)];
        for (int s = 0; s < step_count; ++s) {
            const int nx = x + kSteps[s].dx;
            const int ny = y + kSteps[s].dy;
            if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
            const std::uint16_t c = cost(nx, ny);
            if (c == 0) continue;
            const bool diag = s >= 4;
            if (diag && (cost(nx, y) == 0 || cost(x, ny) == 0)) continue;
            const std::int32_t next = ny * width + nx;
            const std::int32_t pos = heap_pos_[static_cast<std::size_t>(next)];
            if (pos == -2) continue;
            // Moving from `next` toward `cell` enters `cell`: the cost of the cell being entered.
            const std::uint16_t enter = cost(x, y);
            const std::uint32_t step = diag ? (std::uint32_t(enter) * 14u + 5u) / 10u : enter;
            const std::uint32_t nd = saturating_add(d, step);
            if (nd >= dist[static_cast<std::size_t>(next)]) continue; // ties keep the first parent
            dist[static_cast<std::size_t>(next)] = nd;
            next_.cells()[static_cast<std::size_t>(next)] = cell;
            if (pos < 0) {
                heap_[static_cast<std::size_t>(heap_size_)] = next;
                heap_pos_[static_cast<std::size_t>(next)] = heap_size_;
                ++heap_size_;
                sift_up(heap_size_ - 1);
            } else {
                sift_up(pos);
            }
        }
    }
}

std::uint32_t DijkstraMap::at(IVec2 p) const {
    return dist_.get_or(p, kUnreachable);
}

IVec2 DijkstraMap::downhill(IVec2 p) const {
    const std::int32_t n = next_.get_or(p, -1);
    if (n < 0) return p;
    return {n % next_.width(), n / next_.width()};
}

std::optional<IVec2> DijkstraMap::step_toward_goal(IVec2 p) const {
    const IVec2 n = downhill(p);
    if (n == p) return std::nullopt;
    return n;
}

void FlowField::compute(const DijkstraMap &d) {
    const IVec2 size = d.size();
    if (next_.width() != size.x || next_.height() != size.y) next_.resize(size.x, size.y, -1);
    for (int y = 0; y < size.y; ++y) {
        for (int x = 0; x < size.x; ++x) {
            const IVec2 n = d.downhill({x, y});
            next_(x, y) = (n == IVec2{x, y}) ? -1 : n.y * size.x + n.x;
        }
    }
}

IVec2 FlowField::next_cell(IVec2 cell) const {
    const std::int32_t n = next_.get_or(cell, -1);
    if (n < 0) return cell;
    return {n % next_.width(), n / next_.width()};
}

Vec2 FlowField::direction(Vec2 world_pos, const TileWorld &w) const {
    const IVec2 cell = w.tile_at(world_pos);
    const IVec2 n = next_cell(cell);
    if (n == cell) return {};
    return normalize_or(w.tile_center(n) - world_pos, {});
}

} // namespace lam
