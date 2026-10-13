#pragma once
// Path requests, served within a per-tick budget at Simulate -1100, and route following for
// top-down agents. A request that runs out of budget stays pending and the old route is kept.
// Give the request and follow_route the same point (Transform + start_offset).
#include <cstdint>
#include <lam/ecs/component.h>
#include <lam/math/vec2.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_path.h>
namespace lam {
struct PathRequest {                                            // component; removed when served
    IVec2 goal;                                                 // TileWorld cell to reach
    std::uint64_t requested_tick = 0;                           // World::tick() when asked (game bookkeeping)
    GridDirs dirs = GridDirs::Eight;                            // step neighbourhood of the search
    PathSmoothing smoothing = PathSmoothing::LineOfSight;       // Corners for grid-stepping agents
    Vec2 start_offset;                                          // added to Transform.position, e.g. Collider::offset
};
struct PathService {                                            // World resource (init_world)
    PathFinder finder;                                          // sized to TilemapConfig::max_path_cells
    int searches_per_tick = 8;                                  // A* searches per tick, read every tick
};
// Steers toward the next waypoint, skipping ones within arrive_radius of `pos`. Returns a unit
// direction, or zero once the route is used up.
Vec2 follow_route(PathRoute &route, const TileWorld &w, Vec2 pos, float arrive_radius = 2);
// True when the walker should ask again: no route, used up early, a new goal, computed after
// now_tick (undo or load), or older than repath_ticks.
bool route_stale(const PathRoute &route, IVec2 goal, std::uint64_t now_tick, std::uint64_t computed_tick,
                 std::uint32_t repath_ticks);
}
LAM_REFLECT(lam::PathRequest, goal, requested_tick, dirs, smoothing, start_offset)
