#pragma once
// Budgeted A* over cost grids (0 = impassable). Diagonals cost x1.4 and can't cut corners; ties
// always break the same way. No allocation after resize().
#include <array>
#include <cstdint>
#include <vector>
#include <lam/core/data.h>
#include <lam/core/hash.h>
#include <lam/core/result.h>
#include <lam/ecs/component.h>
#include <lam/math/grid.h>
#include <lam/math/vec2.h>
#include <lam/physics/tile_world.h>
namespace lam {
// Step neighbourhood of grid searches: 4 (orthogonal) or 8 (with diagonals).
enum class GridDirs : std::uint8_t { Four, Eight };
// Waypoints a PathRoute holds at most; a longer route is cut (status Partial) and asked again.
inline constexpr int kMaxRoutePoints = 32;
// kDefaultTileCost (the default cost of entering a cell) is in <lam/physics/tile_world.h>.
struct PathRoute {                                              // component; smoothed waypoints
    std::array<IVec2, kMaxRoutePoints> cells{};                 // waypoints after the start cell
    std::uint8_t count = 0;                                     // 0 = no route
    std::uint8_t next = 0;                                      // waypoint being walked to; == count when used up
    bool complete = false;                                      // false: partial route toward the goal
    IVec2 goal;                                                 // the cell it was computed for
    std::uint64_t computed_tick = 0;                            // World::tick() when served (serve_path_requests)
};
// Save ops; reading rejects count > kMaxRoutePoints or next > count.
Status lam_read(PathRoute &m, DataReader &in, const SerializeContext &ctx);
void lam_write(const PathRoute &m, DataWriter &out, const SerializeContext &ctx);
void lam_hash(const PathRoute &m, Hasher &h);
// Partial: goal unreachable (the route ends at the nearest reachable cell), route cut at
// kMaxRoutePoints, or start blocked. NoBudget: out of searches this tick, route untouched.
enum class PathStatus : std::uint8_t { Found, Partial, NoBudget };
// LineOfSight drops waypoints a tile-sized box can slide past (Eight dirs only), for free movers.
// Corners keeps only the turns, so grid-stepping agents can walk them cell by cell.
enum class PathSmoothing : std::uint8_t { LineOfSight, Corners };
// One per world (PathService) or per system. Reuses its buffers. Not thread-safe.
class PathFinder {
public:
    // Storage for grids of up to max_cells cells (<= 0: none). Allocates; load time.
    void resize(int max_cells);
    // Start of a tick: `searches` find() calls may search before NoBudget.
    void begin_tick(int searches);
    // Same cell, a clear line or a blocked start spend no budget. Errors if never sized or the
    // grid is larger than max_cells.
    Result<PathStatus> find(const Grid<std::uint16_t> &cost, IVec2 from, IVec2 goal, GridDirs dirs, PathRoute &route);
    // Cells the last search took off its open list (0 if it answered without searching).
    int last_expansions() const;
    // Searches still allowed this tick.
    int searches_left() const { return searches_left_; }
    // max_cells given to resize().
    int capacity() const { return static_cast<int>(nodes_.size()); }
    // Waypoint policy of later find() calls (default LineOfSight).
    void set_smoothing(PathSmoothing s) { smoothing_ = s; }
    PathSmoothing smoothing() const { return smoothing_; }
private:
    // `stamp` == stamp_ marks the cell as part of the current search, so nothing needs clearing.
    struct Node {
        std::uint32_t stamp = 0;
        std::uint32_t g = 0;
        std::int32_t parent = -1;
        std::int32_t heap_pos = -1;
    };
    // key = (f << 32) | h, so one compare orders by f then h; ties go to the lower cell index.
    struct Open {
        std::uint64_t key = 0;
        std::int32_t cell = 0;
    };
    void heap_sift_up(std::int32_t pos, Open entry);
    void heap_sift_down(std::int32_t pos, Open entry);
    std::int32_t search(const Grid<std::uint16_t> &cost, std::int32_t start, IVec2 goal, GridDirs dirs,
                        std::uint16_t min_cost);
    std::vector<Node> nodes_;
    std::vector<Open> heap_;                                    // open list: binary heap
    std::vector<std::int32_t> trail_;                           // scratch: the cell path, end to start
    std::int32_t heap_size_ = 0;
    std::uint32_t stamp_ = 0;
    int searches_left_ = 0;
    int last_expansions_ = 0;
    PathSmoothing smoothing_ = PathSmoothing::LineOfSight;
};
// True if the segment a-b (cell units) crosses no blocked cell; off-grid counts as blocked.
// Passing exactly through the corner between two diagonal walls counts as blocked.
bool grid_ray_clear(const Grid<std::uint16_t> &cost, Vec2 a, Vec2 b);
// True if a box of half extents `half` can slide from a to b without overlapping a blocked cell
// or one costlier than max_cost. Errs on the blocked side at exact corners.
bool grid_box_clear(const Grid<std::uint16_t> &cost, Vec2 a, Vec2 b, Vec2 half,
                    std::uint16_t max_cost = 0xFFFF);
// The centre of `cell` in cell units ({x + 0.5, y + 0.5}).
constexpr Vec2 cell_center(IVec2 cell) {
    return {static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.y) + 0.5f};
}
}
LAM_REFLECT_ENUM(lam::GridDirs, "four", "eight")
LAM_REFLECT_ENUM(lam::PathSmoothing, "line_of_sight", "corners")
LAM_REFLECT_CUSTOM(lam::PathRoute, lam::kDefaultFlags)
