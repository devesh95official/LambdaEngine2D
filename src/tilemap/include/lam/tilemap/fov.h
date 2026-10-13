#pragma once
// Symmetric shadowcasting FOV (Albert Ford's, exact integer slopes) with explored memory.
// draw_tiles fogs the map when the world has both FovMemory and FogStyle.
#include <cstdint>
#include <lam/core/data.h>
#include <lam/core/hash.h>
#include <lam/ecs/component.h>
#include <lam/math/color.h>
#include <lam/math/grid.h>
#include <lam/physics/tile_world.h>
#include <lam/render/sprite.h>
namespace lam {
// True if `cell` blocks sight; `ctx` is the pointer handed to compute_fov.
using OpaqueFn = bool (*)(const void *ctx, IVec2 cell);
// Marks every cell of `visible` seen from `origin`. Cells outside `visible` still cast shadows.
// radius < 0 sees nothing. No allocation.
void compute_fov(IVec2 origin, int radius, OpaqueFn opaque, const void *ctx, Grid<std::uint8_t> &visible);
struct FovMemory {                                              // World resource; snapshot_resource it
    Grid<std::uint8_t> visible;                                 // 1 = seen now
    Grid<std::uint8_t> explored;                                // 1 = seen at some time
};
// Saved run-length encoded.
Status lam_read(FovMemory &m, DataReader &in, const SerializeContext &ctx);
void lam_write(const FovMemory &m, DataWriter &out, const SerializeContext &ctx);
// Hash op: sizes and every cell of both grids.
void lam_hash(const FovMemory &m, Hasher &h);
// Resizes the grids to `w` if needed (which resets explored), recomputes `visible` and marks it
// explored. Cells set in `extra_opaque` block sight too.
void update_fov(FovMemory &m, const TileWorld &w, IVec2 origin, int radius,
                const Grid<std::uint8_t> *extra_opaque = nullptr);
struct FogStyle {                                               // World resource (optional)
    Color unseen{0, 0, 0, 255};                                 // never explored
    Color explored{0, 0, 0, 140};                               // explored, not visible now
    std::uint8_t layer = kLayerForeground;                      // render layer of the fog quads
};
}
LAM_REFLECT_CUSTOM(lam::FovMemory, lam::kDefaultFlags)
