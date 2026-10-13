// Procedural generation: BSP rooms, cellular caves and region joining. Every random
// choice goes through the caller's Rng in a fixed order, so a seed always gives the same level.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <lam/tilemap/procgen.h>

namespace lam {

namespace {

constexpr std::uint8_t kWall = 0;
constexpr std::uint8_t kFloor = 1;

IVec2 room_center(const IRect &r) {
    return {r.x + r.w / 2, r.y + r.h / 2};
}

// L-shaped corridor from a to b, carving `value`; the bend's side is chosen by rng.
void carve_corridor(Grid<std::uint8_t> &g, Rng &rng, IVec2 a, IVec2 b, std::uint8_t value) {
    const bool horizontal_first = rng.below(2) == 0;
    const IVec2 bend = horizontal_first ? IVec2{b.x, a.y} : IVec2{a.x, b.y};
    const auto line = [&](IVec2 from, IVec2 to) {
        const int sx = (to.x > from.x) - (to.x < from.x);
        const int sy = (to.y > from.y) - (to.y < from.y);
        for (IVec2 p = from;; p = p + IVec2{sx, sy}) {
            g.set(p.x, p.y, value);
            if (p == to) break;
        }
    };
    line(a, bend);
    line(bend, b);
}

struct Bsp {
    Grid<std::uint8_t> &grid;
    Rng &rng;
    const BspParams &p;
    std::vector<IRect> &rooms;

    // Returns the index of a room in this subtree, or -1 if none fitted.
    int split(IRect area, int depth) {
        const int min_room = std::max(p.min_room, 1);
        const int pad = std::max(p.padding, 0);
        const int min_leaf = min_room + 2 * pad;
        const bool can_v = area.w >= 2 * min_leaf;
        const bool can_h = area.h >= 2 * min_leaf;
        if (depth > 0 && (can_v || can_h)) {
            bool vertical = can_v;
            if (can_v && can_h) {
                if (area.w * 4 > area.h * 5)
                    vertical = true;
                else if (area.h * 4 > area.w * 5)
                    vertical = false;
                else
                    vertical = rng.below(2) == 0;
            }
            IRect a = area;
            IRect b = area;
            if (vertical) {
                const int s = rng.between(min_leaf, area.w - min_leaf);
                a.w = s;
                b.x = area.x + s;
                b.w = area.w - s;
            } else {
                const int s = rng.between(min_leaf, area.h - min_leaf);
                a.h = s;
                b.y = area.y + s;
                b.h = area.h - s;
            }
            const int ra = split(a, depth - 1);
            const int rb = split(b, depth - 1);
            if (ra >= 0 && rb >= 0) {
                carve_corridor(grid, rng, room_center(rooms[static_cast<std::size_t>(ra)]),
                               room_center(rooms[static_cast<std::size_t>(rb)]), kFloor);
            }
            return ra >= 0 ? ra : rb;
        }
        const int max_w = std::min(std::max(p.max_room, min_room), area.w - 2 * pad);
        const int max_h = std::min(std::max(p.max_room, min_room), area.h - 2 * pad);
        if (max_w < min_room || max_h < min_room) return -1;
        IRect room;
        room.w = rng.between(min_room, max_w);
        room.h = rng.between(min_room, max_h);
        room.x = rng.between(area.x + pad, area.x + area.w - pad - room.w);
        room.y = rng.between(area.y + pad, area.y + area.h - pad - room.h);
        for (int y = room.y; y < room.y + room.h; ++y) {
            for (int x = room.x; x < room.x + room.w; ++x)
                grid.set(x, y, kFloor);
        }
        rooms.push_back(room);
        return static_cast<int>(rooms.size()) - 1;
    }
};

} // namespace

void bsp_rooms(Grid<std::uint8_t> &grid, Rng &rng, const BspParams &p, std::vector<IRect> &rooms) {
    rooms.clear();
    grid.fill(kWall);
    if (grid.width() < 3 || grid.height() < 3) return;
    Bsp bsp{grid, rng, p, rooms};
    bsp.split(IRect{1, 1, grid.width() - 2, grid.height() - 2}, std::max(p.depth, 0));
}

void cellular_caves(Grid<std::uint8_t> &grid, Rng &rng, float fill, int steps) {
    const int w = grid.width();
    const int h = grid.height();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool border = x == 0 || y == 0 || x == w - 1 || y == h - 1;
            grid(x, y) = (border || rng.chance(fill)) ? kWall : kFloor;
        }
    }
    // Each round computes the new value into bit 1 from bit 0, then shifts: in place, no copy.
    for (int s = 0; s < steps; ++s) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int walls = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (!grid.inside(nx, ny) || (grid(nx, ny) & 1u) == kWall) ++walls;
                    }
                }
                const std::uint8_t old = grid(x, y) & 1u;
                const bool border = x == 0 || y == 0 || x == w - 1 || y == h - 1;
                std::uint8_t now = old;
                if (border || walls >= 5)
                    now = kWall;
                else if (walls <= 3)
                    now = kFloor;
                grid(x, y) = static_cast<std::uint8_t>(old | (now << 1));
            }
        }
        for (std::uint8_t &c : grid.cells())
            c = static_cast<std::uint8_t>(c >> 1);
    }
}

int connect_regions(Grid<std::uint8_t> &grid, Rng &rng, std::uint8_t floor) {
    const int w = grid.width();
    const int h = grid.height();
    const std::size_t n = grid.size();
    std::vector<std::int32_t> label(n, -1);
    std::vector<std::int32_t> queue;
    queue.reserve(n);
    struct Region {
        std::int32_t first;
        std::int32_t size;
        std::int32_t begin; // into `members`
    };
    std::vector<Region> regions;
    std::vector<std::int32_t> members;
    members.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (grid.cells()[i] != floor || label[i] >= 0) continue;
        const auto id = static_cast<std::int32_t>(regions.size());
        Region r{static_cast<std::int32_t>(i), 0, static_cast<std::int32_t>(members.size())};
        queue.clear();
        queue.push_back(static_cast<std::int32_t>(i));
        label[i] = id;
        for (std::size_t q = 0; q < queue.size(); ++q) {
            const std::int32_t c = queue[q];
            members.push_back(c);
            ++r.size;
            const int x = c % w;
            const int y = c / w;
            const IVec2 around[4] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
            for (const IVec2 a : around) {
                if (!grid.inside(a) || grid(a.x, a.y) != floor) continue;
                const std::size_t k = grid.index(a.x, a.y);
                if (label[k] >= 0) continue;
                label[k] = id;
                queue.push_back(static_cast<std::int32_t>(k));
            }
        }
        regions.push_back(r);
    }
    if (regions.size() < 2) return 0;
    std::size_t main = 0;
    for (std::size_t i = 1; i < regions.size(); ++i) {
        if (regions[i].size > regions[main].size) main = i;
    }
    const auto pick = [&](const Region &r) {
        const std::int32_t c = members[static_cast<std::size_t>(r.begin) +
                                       rng.below(static_cast<std::uint32_t>(r.size))];
        return IVec2{c % w, c / w};
    };
    int joined = 0;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (i == main) continue;
        carve_corridor(grid, rng, pick(regions[i]), pick(regions[main]), floor);
        ++joined;
    }
    (void)h;
    return joined;
}

} // namespace lam
