#pragma once
// Grid-step movement for puzzles and roguelikes. Move entities only through try_step or
// set_grid_position so GridIndex stays current. Transform sits on the cell centre.
#include <cstdint>
#include <span>
#include <vector>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/ease.h>
#include <lam/math/grid.h>
#include <lam/math/vec2.h>
#include <lam/physics/components.h>
#include <lam/physics/tile_world.h>
namespace lam {
class World;
// Writing `cell` directly moves neither Transform nor GridIndex; use try_step or set_grid_position.
struct GridPosition {                                           // component; Transform follows the cell centre
    IVec2 cell;                                                 // TileWorld cell
    CollisionMask layer = 1;                                    // tested against StepRules::blocking
    bool blocks = true;                                         // occupies its cell for try_step
};
struct GridMover {                                              // component; tick-time step animation
    IVec2 from, to;                                             // cells of the step being animated
    std::uint16_t ticks = 8;                                    // duration of one step (0 = instant)
    std::uint16_t elapsed = 0;                                  // ticks of the current step done
    Ease ease = Ease::OutQuad;                                  // curve between the two cell centres
    bool moving = false;                                        // a step is under way (try_step answers Busy)
};
class GridIndex {                                               // World resource (init_world)
public:
    // Re-indexes every GridPosition over `tiles`. No allocation once counts are stable.
    void rebuild(World &w, const TileWorld &tiles);
    // Entities on `cell`; empty outside the grid. Valid until the next rebuild or update.
    std::span<const Entity> at(IVec2 cell) const;               // sorted by entity index
    // Moves `e` between cells, inserting it if it wasn't listed in `from`. Pass the cell the index
    // holds or `e` ends up listed twice until the next rebuild.
    void update(Entity e, IVec2 from, IVec2 to);
    IVec2 size() const { return {width_, height_}; }
private:
    struct Cell {
        std::uint32_t begin = 0;
        std::uint32_t count = 0;
        std::uint32_t capacity = 0;
    };
    void insert(Entity e, IVec2 cell);
    void erase(Entity e, IVec2 cell);
    std::vector<Cell> cells_;
    std::vector<Entity> entries_;                               // per-cell runs with slack
    int width_ = 0, height_ = 0;
};
enum class StepResult : std::uint8_t { Moved, BlockedTile, BlockedEntity, Busy };
// What may block a step: GridPosition layers in `blocking`; diagonal steps allowed or not.
struct StepRules { CollisionMask blocking = 0xFFFFFFFFu; bool diagonal = false; };
// Steps `e` by `dir` (each component -1..1) immediately. Busy: still moving, or a bad dir.
// BlockedTile: target off-grid, solid or cost 0. BlockedEntity: *blocker gets the lowest index.
StepResult try_step(World &w, Entity e, IVec2 dir, const StepRules &rules = {}, Entity *blocker = nullptr);
// Dry run of try_step. For undo: check, push the history, then try_step.
StepResult can_step(const World &w, Entity e, IVec2 dir, const StepRules &rules = {},
                    Entity *blocker = nullptr);
// Moves `e` to `cell` without any checks. snap jumps Transform and stops the GridMover; otherwise
// a GridMover tweens from the old cell.
void set_grid_position(World &w, Entity e, IVec2 cell, bool snap = true);
// Inserts TileWorld and GridIndex if missing, for tests and tools that skip TilemapPlugin.
void install_grid_resources(World &w);
// System bodies registered by TilemapPlugin, public for tests.
// Simulate -1500: GridIndex::rebuild.
void grid_index_rebuild(World &w);
// Animate -1200: steps every moving GridMover and holds TurnScheduler::busy while any move.
void grid_movers(World &w);
}
LAM_REFLECT(lam::GridPosition, cell, layer, blocks)
LAM_REFLECT(lam::GridMover, from, to, ticks, elapsed, ease, moving)
