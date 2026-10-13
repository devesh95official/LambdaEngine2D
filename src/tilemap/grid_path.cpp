// A* over cost grids. The node table is stamped instead of
// cleared and the trail is preallocated, so nothing allocates after resize(). Integer costs only.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>

#include <lam/tilemap/grid_path.h>

namespace lam {

namespace {

// Cell coordinates are held to this range before they become an int, so an absurd or non-finite
// position cannot overflow one.
constexpr float kCellLimit = 1000000.0f;
// grid_box_clear() finds where a slanted sweep enters and leaves a row with one division, which
// can be off by a rounding step. Widen by this much so exact corner hits come out "blocked".
constexpr float kSweepSlack = 1.0f / 16384.0f;
// Up to this many cells a side, LineOfSight smoothing proves a run of cells clear by testing their
// bounding rectangle once. Division error stays well under 1/side cell there, so a clear
// rectangle implies every individual slide is clear. Bigger grids test cell by cell.
constexpr int kBlockMaxSide = 1024;
// Only try a block while its rectangle has at most this many cells per cell of the slides it
// replaces. The scan is cheap, but not worth it once the rectangle covers far-off walls.
constexpr std::int64_t kBlockAreaRatio = 4;

int cell_of(float v) {
    const float c = std::floor(v);
    if (!(c > -kCellLimit)) return -static_cast<int>(kCellLimit); // also NaN
    if (c > kCellLimit) return static_cast<int>(kCellLimit);
    return static_cast<int>(c);
}

bool finite(Vec2 v) {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

bool blocked(const Grid<std::uint16_t> &cost, int x, int y) {
    return cost.get_or(x, y, 0) == 0;
}

std::uint32_t diagonal_cost(std::uint32_t c) {
    return (c * 14u + 5u) / 10u;
}

std::uint32_t saturating_add(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t s = a + b;
    return s < a ? 0xFFFFFFFFu : s;
}

// The eight steps, straight ones first. The order only decides which of two equally good parents
// a cell keeps; it is fixed so the same question always gives the same route.
struct Step {
    int dx;
    int dy;
};
constexpr Step kSteps[8] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};

void clear_route(PathRoute &route, IVec2 goal) {
    route.count = 0;
    route.next = 0;
    route.complete = false;
    route.goal = goal;
}

// Is any cell of row `ty` blocked (or costlier than max_cost) between columns [low, high)?
bool row_blocked(const Grid<std::uint16_t> &cost, int ty, float low, float high,
                 std::uint16_t max_cost) {
    const int first = cell_of(low);
    const int last = cell_of(std::ceil(high) - 1.0f);
    // Reaching beyond the grid is touching something blocked; saying so here also keeps the loop
    // inside the grid whatever the coordinates are.
    if (first < 0 || last >= cost.width()) return true;
    for (int tx = first; tx <= last; ++tx) {
        const std::uint16_t c = cost(tx, ty);
        if (c == 0 || c > max_cost) return true;
    }
    return false;
}

// Every cell of the rectangle [low, high] (cells, both corners included) passable and no costlier
// than max_cost.
bool rect_clear(const Grid<std::uint16_t> &cost, IVec2 low, IVec2 high, std::uint16_t max_cost) {
    const int width = cost.width();
    const std::uint16_t *c = cost.cells().data();
    for (int y = low.y; y <= high.y; ++y) {
        const std::uint16_t *row = c + static_cast<std::ptrdiff_t>(y) * width;
        // Blocked (0 wraps to 0xFFFF) or costlier than max_cost: cost - 1 >= max_cost. No early
        // exit inside a row, so the loop vectorises.
        unsigned bad = 0;
        for (int x = low.x; x <= high.x; ++x)
            bad |= static_cast<unsigned>(static_cast<std::uint16_t>(row[x] - 1u) >= max_cost);
        if (bad != 0) return false;
    }
    return true;
}

int sign(int v) {
    return (v > 0) - (v < 0);
}

} // namespace

// Amanatides-Woo walk, counted in whole steps so it ends after exactly |dx| + |dy| cell changes.
// A point on a boundary belongs to the cell on its positive side.
bool grid_ray_clear(const Grid<std::uint16_t> &cost, Vec2 a, Vec2 b) {
    if (!finite(a) || !finite(b)) return false;
    int tx = cell_of(a.x);
    int ty = cell_of(a.y);
    const int end_x = cell_of(b.x);
    const int end_y = cell_of(b.y);
    // Both ends inside the grid means every cell between them is inside too, which bounds the walk.
    if (blocked(cost, tx, ty) || blocked(cost, end_x, end_y)) return false;

    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const int step_x = dx > 0.0f ? 1 : -1;
    const int step_y = dy > 0.0f ? 1 : -1;
    int left_x = std::abs(end_x - tx);
    int left_y = std::abs(end_y - ty);

    // t (0 at a, 1 at b) at which the segment reaches the next cell boundary on each axis, worked
    // out afresh from the cell each time, so a ray through an exact corner is seen as one.
    const auto reach_x = [&]() {
        return (static_cast<float>(step_x > 0 ? tx + 1 : tx) - a.x) / dx;
    };
    const auto reach_y = [&]() {
        return (static_cast<float>(step_y > 0 ? ty + 1 : ty) - a.y) / dy;
    };

    while (left_x > 0 || left_y > 0) {
        bool move_x = left_x > 0;
        bool move_y = left_y > 0;
        if (move_x && move_y) {
            const float t_x = reach_x();
            const float t_y = reach_y();
            move_x = t_x <= t_y;
            move_y = t_y <= t_x;
            // Exactly through a corner: it may brush one of the two cells beside it, but not pass
            // between a pair that meets diagonally.
            if (move_x && move_y && blocked(cost, tx + step_x, ty) &&
                blocked(cost, tx, ty + step_y)) {
                return false;
            }
        }
        if (move_x) {
            tx += step_x;
            --left_x;
        }
        if (move_y) {
            ty += step_y;
            --left_y;
        }
        if (blocked(cost, tx, ty)) return false;
    }
    return true;
}

// Row by row: the part of the slide during which the box overlaps the row gives the stretch of
// the row it covers. Exact convex sweep, costing only the cells it touches.
bool grid_box_clear(const Grid<std::uint16_t> &cost, Vec2 a, Vec2 b, Vec2 half,
                    std::uint16_t max_cost) {
    if (!finite(a) || !finite(b) || !finite(half) || half.x < 0.0f || half.y < 0.0f) return false;
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float top = std::min(a.y, b.y) - half.y;
    const float bottom = std::max(a.y, b.y) + half.y;
    const int first_row = cell_of(top);
    const int last_row = cell_of(std::ceil(bottom) - 1.0f);
    if (first_row < 0 || last_row >= cost.height()) return false; // beyond the grid is blocked

    for (int ty = first_row; ty <= last_row; ++ty) {
        // The box [y - hy, y + hy) overlaps the row [ty, ty + 1) while ty - hy < y < ty + 1 + hy.
        const float y_low = static_cast<float>(ty) - half.y;
        const float y_high = static_cast<float>(ty) + 1.0f + half.y;
        float t0 = 0.0f;
        float t1 = 1.0f;
        if (dy == 0.0f) {
            if (!(a.y > y_low && a.y < y_high)) continue;
        } else {
            const float enter = ((dy > 0.0f ? y_low : y_high) - a.y) / dy;
            const float leave = ((dy > 0.0f ? y_high : y_low) - a.y) / dy;
            t0 = std::max(t0, enter);
            t1 = std::min(t1, leave);
            if (!(t0 < t1)) continue;
        }
        // End points are used as given, so a box resting exactly against a wall is not pushed into
        // it by the slack; only positions worked out by division get it.
        const float x0 = t0 <= 0.0f ? a.x : a.x + dx * t0;
        const float x1 = t1 >= 1.0f ? b.x : a.x + dx * t1;
        const float slack0 = (t0 <= 0.0f || dx == 0.0f) ? 0.0f : kSweepSlack;
        const float slack1 = (t1 >= 1.0f || dx == 0.0f) ? 0.0f : kSweepSlack;
        const float low = std::min(x0 - slack0, x1 - slack1) - half.x;
        const float high = std::max(x0 + slack0, x1 + slack1) + half.x;
        if (row_blocked(cost, ty, low, high, max_cost)) return false;
    }
    return true;
}

void PathFinder::resize(int max_cells) {
    const std::size_t count = max_cells > 0 ? static_cast<std::size_t>(max_cells) : 0;
    nodes_.assign(count, Node{});
    heap_.assign(count, Open{});
    trail_.assign(count, 0);
    heap_size_ = 0;
    stamp_ = 0;
    last_expansions_ = 0;
}

void PathFinder::begin_tick(int searches) {
    searches_left_ = searches;
}

int PathFinder::last_expansions() const {
    return last_expansions_;
}

namespace {

// Open-list order: lower f, then lower h (the cell nearer the goal), then the lower cell index.
inline bool open_before(std::uint64_t ka, std::int32_t ca, std::uint64_t kb, std::int32_t cb) {
    return static_cast<bool>(static_cast<int>(ka < kb) |
                             (static_cast<int>(ka == kb) & static_cast<int>(ca < cb)));
}

inline std::uint64_t open_key(std::uint64_t f, std::uint32_t h) {
    return (std::min<std::uint64_t>(f, 0xFFFFFFFFull) << 32) | h;
}

} // namespace

// Both sifts move a "hole" instead of swapping, and keep Node::heap_pos current for decrease-key.
void PathFinder::heap_sift_up(std::int32_t pos, Open entry) {
    Open *heap = heap_.data();
    Node *nodes = nodes_.data();
    while (pos > 0) {
        const std::int32_t parent = (pos - 1) / 2;
        const Open other = heap[parent];
        if (!open_before(entry.key, entry.cell, other.key, other.cell)) break;
        heap[pos] = other;
        nodes[other.cell].heap_pos = pos;
        pos = parent;
    }
    heap[pos] = entry;
    nodes[entry.cell].heap_pos = pos;
}

void PathFinder::heap_sift_down(std::int32_t pos, Open entry) {
    Open *heap = heap_.data();
    Node *nodes = nodes_.data();
    const std::int32_t size = heap_size_;
    for (;;) {
        std::int32_t child = pos * 2 + 1;
        if (child >= size) break;
        if (child + 1 < size) {
            child += static_cast<std::int32_t>(open_before(
                heap[child + 1].key, heap[child + 1].cell, heap[child].key, heap[child].cell));
        }
        const Open other = heap[child];
        if (!open_before(other.key, other.cell, entry.key, entry.cell)) break;
        heap[pos] = other;
        nodes[other.cell].heap_pos = pos;
        pos = child;
    }
    heap[pos] = entry;
    nodes[entry.cell].heap_pos = pos;
}

std::int32_t PathFinder::search(const Grid<std::uint16_t> &cost, std::int32_t start, IVec2 goal,
                                GridDirs dirs, std::uint16_t min_cost) {
    const int width = cost.width();
    const int height = cost.height();
    ++stamp_;
    if (stamp_ == 0) {
        // The counter wrapped: old marks could now be mistaken for this search's.
        std::fill(nodes_.begin(), nodes_.end(), Node{});
        stamp_ = 1;
    }
    // The heuristic aims at the goal held just outside the grid, so a far-away goal neither
    // overflows nor changes which reachable cell is nearest.
    const int gx = std::clamp(goal.x, -1, width);
    const int gy = std::clamp(goal.y, -1, height);
    const std::uint64_t straight = min_cost;
    const std::uint64_t diagonal = diagonal_cost(min_cost);
    const bool four = dirs == GridDirs::Four;
    const auto heuristic = [&](int x, int y) -> std::uint32_t {
        const std::uint64_t ddx = static_cast<std::uint64_t>(std::abs(x - gx));
        const std::uint64_t ddy = static_cast<std::uint64_t>(std::abs(y - gy));
        const std::uint64_t h =
            four ? straight * (ddx + ddy)
                 : straight * std::max(ddx, ddy) + (diagonal - straight) * std::min(ddx, ddy);
        return h > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(h);
    };
    const std::int32_t goal_index =
        (cost.inside(goal) && cost(goal.x, goal.y) != 0) ? goal.y * width + goal.x : -1;
    const std::uint16_t *c = cost.cells().data();
    Node *nodes = nodes_.data();
    const std::uint32_t stamp = stamp_;

    Node &first = nodes[start];
    first.stamp = stamp;
    first.g = 0;
    first.parent = -1;
    const std::uint32_t start_h = heuristic(start % width, start / width);
    heap_size_ = 1;
    heap_sift_up(0, Open{open_key(start_h, start_h), start});

    // The reached cell nearest the goal, for the case that the goal itself is never reached.
    std::int32_t best = start;
    std::uint32_t best_h = start_h;
    // Neighbour offsets in kSteps order.
    std::int32_t offset[8];
    for (int s = 0; s < 8; ++s)
        offset[s] = kSteps[s].dy * width + kSteps[s].dx;
    const int step_count = four ? 4 : 8;

    // The heuristic is consistent, so a cell taken off the open list has its final cost and is
    // never opened again: at most one expansion per cell.
    int expansions = 0;
    while (heap_size_ > 0) {
        const Open top = heap_[0];
        nodes[top.cell].heap_pos = -1;
        --heap_size_;
        if (heap_size_ > 0) heap_sift_down(0, heap_[static_cast<std::size_t>(heap_size_)]);
        const std::int32_t cell = top.cell;
        ++expansions;
        if (cell == goal_index) {
            last_expansions_ = expansions;
            return cell;
        }
        const std::uint32_t here_h = static_cast<std::uint32_t>(top.key);
        if (here_h < best_h || (here_h == best_h && cell < best)) {
            best = cell;
            best_h = here_h;
        }
        const int x = cell % width;
        const int y = cell / width;
        const std::uint32_t g = nodes[cell].g;
        // Which of the eight neighbours exist (no step leaves the grid).
        const bool e = x + 1 < width, w = x > 0, s = y + 1 < height, n = y > 0;
        const bool inside[8] = {e, w, s, n, s && e, s && w, n && e, n && w};
        for (int k = 0; k < step_count; ++k) {
            if (!inside[k]) continue;
            const std::int32_t next = cell + offset[k];
            const std::uint16_t step_cost = c[next];
            if (step_cost == 0) continue;
            const bool diag = k >= 4;
            // No corner cutting: a body has width, so both cells beside a diagonal step must be
            // passable. This also closes a one-cell diagonal gap.
            if (diag && (c[cell + kSteps[k].dx] == 0 || c[cell + kSteps[k].dy * width] == 0))
                continue;
            Node &node = nodes[next];
            const std::uint32_t ng = saturating_add(g, diag ? diagonal_cost(step_cost) : step_cost);
            if (node.stamp != stamp) {
                node.stamp = stamp;
                node.g = ng;
                node.parent = cell;
                const std::uint32_t h = heuristic(x + kSteps[k].dx, y + kSteps[k].dy);
                ++heap_size_;
                heap_sift_up(heap_size_ - 1, Open{open_key(std::uint64_t(ng) + h, h), next});
            } else if (node.heap_pos >= 0 && ng < node.g) {
                node.g = ng;
                node.parent = cell;
                const std::uint32_t h =
                    static_cast<std::uint32_t>(heap_[static_cast<std::size_t>(node.heap_pos)].key);
                heap_sift_up(node.heap_pos, Open{open_key(std::uint64_t(ng) + h, h), next});
            }
        }
    }
    last_expansions_ = expansions;
    return best;
}

Result<PathStatus> PathFinder::find(const Grid<std::uint16_t> &cost, IVec2 from, IVec2 goal,
                                    GridDirs dirs, PathRoute &route) {
    if (nodes_.empty()) {
        return make_error(ErrorCode::Invalid,
                          "PathFinder::find: the finder was never sized (call resize first)");
    }
    const std::size_t cells = cost.size();
    if (cells > nodes_.size()) {
        return make_error(ErrorCode::OutOfRange,
                          "PathFinder::find: a grid of " + std::to_string(cells) +
                              " cells exceeds max_cells " + std::to_string(nodes_.size()));
    }
    // Questions that need no search are answered whatever is left of the budget.
    if (cells == 0 || !cost.inside(from) || cost(from.x, from.y) == 0) {
        last_expansions_ = 0;
        clear_route(route, goal);
        return PathStatus::Partial;
    }
    const PathSmoothing smoothing = dirs == GridDirs::Four ? PathSmoothing::Corners : smoothing_;
    const bool goal_open = cost.inside(goal) && cost(goal.x, goal.y) != 0;
    bool plain = from == goal;
    if (!plain && smoothing == PathSmoothing::LineOfSight && goal_open) {
        const std::uint16_t limit = std::max(cost(from.x, from.y), cost(goal.x, goal.y));
        plain = grid_box_clear(cost, cell_center(from), cell_center(goal), {0.5f, 0.5f}, limit);
    }
    if (plain) {
        last_expansions_ = 0;
        clear_route(route, goal);
        route.cells[0] = goal;
        route.count = 1;
        route.complete = true;
        return PathStatus::Found;
    }
    if (searches_left_ <= 0) return PathStatus::NoBudget;
    --searches_left_;
    last_expansions_ = 0;

    std::uint16_t min_cost = 0xFFFF;
    for (const std::uint16_t c : cost.cells()) {
        if (c != 0 && c < min_cost) min_cost = c;
    }
    const int width = cost.width();
    const std::int32_t start = from.y * width + from.x;
    const std::int32_t end = search(cost, start, goal, dirs, min_cost);
    const bool reached = goal_open && end == goal.y * width + goal.x;

    // The cells from the end back to the start; trail_[length - 1] is the start.
    std::int32_t length = 0;
    for (std::int32_t c = end; c >= 0; c = nodes_[static_cast<std::size_t>(c)].parent) {
        trail_[static_cast<std::size_t>(length)] = c;
        ++length;
    }
    const auto at = [&](std::int32_t i) { // i counts from the start (0) to the end (length - 1)
        const std::int32_t c = trail_[static_cast<std::size_t>(length - 1 - i)];
        return IVec2{c % width, c / width};
    };

    clear_route(route, goal);
    bool cut = false;
    const auto keep = [&](IVec2 c) {
        if (route.count == kMaxRoutePoints) {
            cut = true;
            return false;
        }
        route.cells[route.count] = c;
        ++route.count;
        return true;
    };
    if (length == 1) {
        keep(at(0)); // nowhere to go: the walker stays on its cell
    } else if (smoothing == PathSmoothing::Corners) {
        for (std::int32_t i = 1; i < length && !cut; ++i) {
            const IVec2 here = at(i);
            if (i + 1 < length) {
                const IVec2 d0 = here - at(i - 1);
                const IVec2 d1 = at(i + 1) - here;
                if (sign(d0.x) == sign(d1.x) && sign(d0.y) == sign(d1.y)) continue;
            }
            keep(here);
        }
    } else {
        // String pulling from `anchor`: keep a cell only if a tile-sized box can't slide straight
        // past it through cells no costlier than the path it replaces. Testing each cell is
        // quadratic on long winding routes, so first try runs of cells as a block (doubling while
        // they pass) using their bounding rectangle, then fall back to one cell at a time.
        // Same waypoints as the plain per-cell loop.
        const bool blocks = width <= kBlockMaxSide && cost.height() <= kBlockMaxSide;
        IVec2 anchor = at(0);
        std::uint16_t stretch = cost(anchor.x, anchor.y);
        std::int32_t block = 2; // candidates tried at once; 0: per cell until the next waypoint
        std::int32_t i = 1;
        while (i < length && !cut) {
            if (blocks && block >= 2) {
                // Candidates i .. i + k - 1 slide to cells i + 1 .. i + k: k <= length - 1 - i.
                std::int32_t k = std::min(block, length - 1 - i);
                bool skipped = false;
                while (k >= 2) {
                    // The first candidate's cost limit; later candidates' limits are never lower.
                    const IVec2 first_after = at(i + 1);
                    const std::uint16_t limit = std::max(
                        {stretch, cost(at(i).x, at(i).y), cost(first_after.x, first_after.y)});
                    IVec2 low = anchor;
                    IVec2 high = anchor;
                    for (std::int32_t j = i + 1; j <= i + k; ++j) {
                        const IVec2 c = at(j);
                        low = {std::min(low.x, c.x), std::min(low.y, c.y)};
                        high = {std::max(high.x, c.x), std::max(high.y, c.y)};
                    }
                    const std::int64_t area =
                        std::int64_t(high.x - low.x + 1) * (high.y - low.y + 1);
                    const std::int64_t per_cell =
                        std::abs(first_after.x - anchor.x) + std::abs(first_after.y - anchor.y) + 1;
                    if (area <= kBlockAreaRatio * per_cell * k &&
                        rect_clear(cost, low, high, limit)) {
                        for (std::int32_t j = i; j < i + k; ++j) {
                            const IVec2 c = at(j);
                            stretch = std::max(stretch, cost(c.x, c.y));
                        }
                        i += k;
                        block = k * 2;
                        skipped = true;
                        break;
                    }
                    k /= 2;
                }
                if (skipped) continue;
                block = 0;
            }
            const IVec2 here = at(i);
            stretch = std::max(stretch, cost(here.x, here.y));
            if (i + 1 < length) {
                const IVec2 after = at(i + 1);
                const std::uint16_t limit = std::max(stretch, cost(after.x, after.y));
                if (grid_box_clear(cost, cell_center(anchor), cell_center(after), {0.5f, 0.5f},
                                   limit)) {
                    ++i;
                    continue;
                }
            }
            if (!keep(here)) break;
            anchor = here;
            stretch = cost(here.x, here.y);
            block = 2;
            ++i;
        }
    }
    route.complete = reached && !cut;
    return route.complete ? PathStatus::Found : PathStatus::Partial;
}

// ---- PathRoute save ops ------------------------------------------------------------------------

namespace {

// PathRoute's fields as LAM_REFLECT listed them, so saves and hashes keep their format; the custom
// ops add the bounds check field reflection cannot express.
const TypeLayout &path_route_layout() {
    static_assert(std::is_standard_layout_v<PathRoute>, "offsetof needs a standard layout");
    static const FieldInfo kFields[] = {
        detail::field_info<decltype(PathRoute::cells)>("cells", offsetof(PathRoute, cells)),
        detail::field_info<decltype(PathRoute::count)>("count", offsetof(PathRoute, count)),
        detail::field_info<decltype(PathRoute::next)>("next", offsetof(PathRoute, next)),
        detail::field_info<decltype(PathRoute::complete)>("complete",
                                                          offsetof(PathRoute, complete)),
        detail::field_info<decltype(PathRoute::goal)>("goal", offsetof(PathRoute, goal)),
        detail::field_info<decltype(PathRoute::computed_tick)>("computed_tick",
                                                               offsetof(PathRoute, computed_tick)),
    };
    static const TypeLayout kLayout{"PathRoute", static_cast<std::uint32_t>(sizeof(PathRoute)),
                                    std::span<const FieldInfo>(kFields)};
    return kLayout;
}

} // namespace

Status lam_read(PathRoute &m, DataReader &in, const SerializeContext &ctx) {
    LAM_TRY(read_fields(path_route_layout(), &m, in, ctx));
    if (m.count > kMaxRoutePoints || m.next > m.count) {
        const std::string why = "count must be 0.." + std::to_string(kMaxRoutePoints) +
                                " and next 0..count (count " + std::to_string(m.count) + ", next " +
                                std::to_string(m.next) + ")";
        in.fail(why);
        return make_error(ErrorCode::Parse, "PathRoute: " + in.path() + ": " + why);
    }
    return {};
}

void lam_write(const PathRoute &m, DataWriter &out, const SerializeContext &ctx) {
    out.begin_object();
    write_fields(path_route_layout(), &m, out, ctx);
    out.end_object();
}

void lam_hash(const PathRoute &m, Hasher &h) {
    hash_fields(path_route_layout(), &m, h);
}

} // namespace lam
