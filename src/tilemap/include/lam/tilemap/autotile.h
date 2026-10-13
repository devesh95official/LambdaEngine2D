#pragma once
// Autotiling with 4-bit edge masks or the 47-tile blob set. Tiles are consecutive GIDs from
// `first`: Edge4 = first + mask * variants + v, Blob47 = first + blob47_index(mask) * variants + v,
// Plain = first + v, with v = tile_hash(x, y, seed) % variants.
#include <cstdint>
#include <span>
#include <vector>
#include <lam/core/id.h>
#include <lam/math/grid.h>
#include <lam/tilemap/tileset.h>
namespace lam {
// Plain: variants only, no edges.
enum class AutotileMode : std::uint8_t { Edge4, Blob47, Plain };
// edge_mask4 bits: the sides whose neighbour is NOT the same terrain.
inline constexpr int kEdgeNorth = 1;
inline constexpr int kEdgeEast = 2;
inline constexpr int kEdgeSouth = 4;
inline constexpr int kEdgeWest = 8;
// blob_mask47 bits: the neighbours that ARE the same terrain; a corner counts only when both of
// its sides do, which leaves 47 distinct masks.
inline constexpr int kBlobN = 1, kBlobNE = 2, kBlobE = 4, kBlobSE = 8, kBlobS = 16, kBlobSW = 32, kBlobW = 64,
                     kBlobNW = 128;
// How the cells of one terrain value become tiles.
struct AutotileRule {
    StrId terrain;                                              // terrain name (informational; native maps)
    std::uint8_t terrain_value = 0;                             // value in the terrain grid
    AutotileMode mode = AutotileMode::Edge4;                    // how neighbours pick the tile
    TileId first = 0;                                           // first GID of the rule's tiles
    std::uint8_t variants = 1;                                  // tiles per mask (picked by tile_hash)
    std::vector<std::uint8_t> blends;                           // terrain values treated as same
    bool edge_outside = true;                                   // outside the grid counts as the same terrain
    std::vector<TileId> details;                                // decorations for autotile_details
    std::uint8_t detail_percent = 0;                            // share of the terrain's cells decorated, 0..100
};
// The Edge4 mask at p: bits for sides whose value differs from `same` (outside the grid counts as
// same when outside_same). No allocation.
int edge_mask4(const Grid<std::uint8_t> &terrain, IVec2 p, std::uint8_t same, bool outside_same);
// The reduced 8-bit blob mask at p (one of 47 values; see kBlobN..kBlobNW). No allocation.
int blob_mask47(const Grid<std::uint8_t> &terrain, IVec2 p, std::uint8_t same, bool outside_same);
// Rank 0..46 of a reduced blob mask in ascending mask order (0 = isolated, 46 = 255 = surrounded);
// -1 if `mask` is not reduced (a corner bit without both of its sides).
int blob47_index(int mask);
// Resizes `out` to the terrain's size if needed (allocates), then tiles every cell.
void autotile(const Grid<std::uint8_t> &terrain, std::span<const AutotileRule> rules, std::uint32_t seed,
              Grid<TileId> &out);
// Re-resolves `cell` and its 8 neighbours after the terrain changed. `out` must match in size.
void autotile_cell(const Grid<std::uint8_t> &terrain, std::span<const AutotileRule> rules, std::uint32_t seed,
                   IVec2 cell, Grid<TileId> &out);
// Per-cell hash used to pick variants.
std::uint32_t tile_hash(int x, int y, std::uint32_t seed);
// Puts one of each rule's `details` on about detail_percent of its cells, skipping `blocked`
// cells. Resizes `out` if needed.
void autotile_details(const Grid<std::uint8_t> &terrain, std::span<const AutotileRule> rules, std::uint32_t seed,
                      Grid<TileId> &out, const Grid<std::uint8_t> *blocked = nullptr);
}
