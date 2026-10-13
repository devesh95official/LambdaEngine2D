#pragma once
// Procedural generation. Grids use 0 = wall, 1 = floor; output depends only on the Rng state.
#include <cstdint>
#include <vector>
#include <lam/math/grid.h>
#include <lam/math/rect.h>
#include <lam/math/rng.h>
namespace lam {
// Room sides in cells, split depth (up to 2^depth rooms), wall cells between a room and its leaf.
struct BspParams { int min_room = 4; int max_room = 10; int depth = 5; int padding = 1; };
// Fills `grid` with walls, carves a room per BSP leaf and joins siblings with L-shaped corridors,
// so every room is reachable. `rooms` receives them in carve order.
void bsp_rooms(Grid<std::uint8_t> &grid, Rng &rng, const BspParams &p, std::vector<IRect> &rooms);
// Random fill, then `steps` rounds of the 4-5 rule (outside counts as wall). In place.
void cellular_caves(Grid<std::uint8_t> &grid, Rng &rng, float fill, int steps);
// Joins every 4-connected floor region to the largest one with L-shaped corridors.
int connect_regions(Grid<std::uint8_t> &grid, Rng &rng, std::uint8_t floor = 1);   // regions joined
}
