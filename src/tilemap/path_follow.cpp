// Route following and staleness. A walker steers toward the world
// centre of the next waypoint.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <lam/tilemap/path_follow.h>

namespace lam {

Vec2 follow_route(PathRoute &route, const TileWorld &w, Vec2 pos, float arrive_radius) {
    // Never past the waypoint storage, whatever count says (game code, inspector, old saves).
    const std::uint8_t n =
        std::min<std::uint8_t>(route.count, static_cast<std::uint8_t>(kMaxRoutePoints));
    while (route.next < n) {
        const Vec2 target = w.tile_center(route.cells[route.next]);
        const float dx = target.x - pos.x;
        const float dy = target.y - pos.y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance > arrive_radius) return Vec2{dx / distance, dy / distance};
        ++route.next;
    }
    return Vec2{};
}

bool route_stale(const PathRoute &route, IVec2 goal, std::uint64_t now_tick,
                 std::uint64_t computed_tick, std::uint32_t repath_ticks) {
    if (route.count == 0) return true;
    if (route.next >= route.count && !route.complete) return true;
    if (route.goal != goal) return true;
    if (now_tick < computed_tick)
        return true; // computed "in the future": time went back (undo, load)
    return now_tick - computed_tick >= repath_ticks;
}

} // namespace lam
