#pragma once
// Dijkstra maps and flow fields for crowds and roguelike AI: compute once per turn, then any
// number of agents walk downhill without searching. No allocation after resize().
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include <lam/math/grid.h>
#include <lam/math/vec2.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_path.h>
namespace lam {
// Cost to the nearest goal for every cell, with the same step costs as PathFinder.
class DijkstraMap {
public:
    // Storage for a grid of `size` cells. Allocates; load time.
    void resize(IVec2 size);
    // Multi-source Dijkstra over `cost` (0 = blocked). Blocked or off-grid goals are skipped.
    void compute(const Grid<std::uint16_t> &cost, std::span<const IVec2> goals, GridDirs dirs);
    std::uint32_t at(IVec2 p) const;                            // UINT32_MAX = unreachable
    // Next cell on a cheapest path to the nearest goal; p itself at a goal, on an unreachable
    // cell or off the grid. Ties always break the same way.
    IVec2 downhill(IVec2 p) const;
    // downhill(p) when it differs from p; nullopt at a goal or where no goal can be reached.
    std::optional<IVec2> step_toward_goal(IVec2 p) const;
    IVec2 size() const { return {dist_.width(), dist_.height()}; }   // the resize() size
    GridDirs dirs() const { return dirs_; }                     // neighbourhood of the last compute()
private:
    bool heap_before(std::int32_t a, std::int32_t b) const;
    void sift_up(std::int32_t pos);
    void sift_down(std::int32_t pos);
    Grid<std::uint32_t> dist_;
    Grid<std::int32_t> next_;                                   // downhill cell index per cell, -1: none
    std::vector<std::int32_t> heap_;                            // open cells (binary heap)
    std::vector<std::int32_t> heap_pos_;                        // per cell: index in heap_, -1 if not open
    std::int32_t heap_size_ = 0;
    GridDirs dirs_ = GridDirs::Eight;
};
// Per-cell steering toward a DijkstraMap's goals, for crowds of free-moving agents.
class FlowField {
public:
    // One downhill target per cell of `d`. Allocates only when the size changes.
    void compute(const DijkstraMap &d);
    // Unit vector toward the centre of the downhill neighbour of world_pos's cell, zero if none.
    // `w` must match the DijkstraMap's grid.
    Vec2 direction(Vec2 world_pos, const TileWorld &w) const;
    // The downhill cell of `cell`, or `cell` itself where there is none.
    IVec2 next_cell(IVec2 cell) const;
private:
    Grid<std::int32_t> next_;                                   // cell index of the downhill neighbour, -1: none
};
}
