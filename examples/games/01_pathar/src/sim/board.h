#pragma once
// The puzzle as ECS state, the authoritative game state of a Pathar world.
// - The player and each stone are entities with GridPosition + GridMover. A push is "try_step the
//   stone, then the player"; try_step keeps GridIndex current and starts the glide.
// - TileWorld is derived: walls and open water are Solid, ground and filled water Empty.
//   sync_tiles() rebuilds it from Board + PuzzleState::filled and is also the restore hook.
// - Counters and flags live in the snapshotted PuzzleState.
// - UndoStack holds a History of whole-World snapshots and is not snapshotted itself.
// Functions take World& so tests can drive a bare World without an App.
#include <array>
#include <cstdint>
#include <vector>
#include <lam/core/id.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/world.h>
#include <lam/math/vec2.h>
#include "sim/rules.h"

namespace pathar {

inline constexpr int kTileSize = 16;           // pixels per cell (TileWorld::tile_size)
inline constexpr std::uint16_t kGlideTicks = 7; // one step: 7 ticks at 60 Hz (~0.11 s)
inline constexpr int kMaxPath = 96;            // click-to-walk route length

struct Player {};                              // tag: the one player entity
struct Stone { std::uint8_t index = 0; };      // the Level::boxes entry it started on
struct Sunk {};                                // tag: the stone filled a water cell (no longer blocks)
struct Facing { Dir dir = Dir::Down; };        // the player's last step (animation picks the clip)

// Static per level, not snapshotted: the parsed level and tables derived from it.
struct Board {
    Level level;
    std::vector<std::uint8_t> dead;            // dead_squares(level)
    int index = -1;                            // global level index (-1 = none loaded)
    std::uint32_t serial = 0;                  // ++ on every load_level (presentation rebuilds on change)
    lam::Vec2 origin;                          // world position of cell (0, 0)'s top-left corner
    lam::StrId footstep = lam::str_id("footstep_stone");   // PlaySound group for a plain step (theme)
    bool quiet = false;                        // emit no PlaySound (the title's self-playing demo)
};

// Everything undo, saves and the state hash must cover besides the entities.
struct PuzzleState {
    std::int32_t level = -1;
    std::int32_t moves = 0;
    std::int32_t pushes = 0;
    std::uint64_t filled = 0;                  // bit i: Level::water[i] holds a sunk stone
    bool solved = false;
    bool helped = false;                       // a hint was used (one star)
    bool stuck_said = false;                   // the "undo" toast was shown for this dead end
    std::uint8_t queued = 0;                   // buffered direction + 1 (0 = none)
    std::array<std::uint8_t, kMaxPath> path{}; // click-to-walk route (Dir values)
    std::uint8_t path_len = 0, path_pos = 0;
    std::uint32_t solved_tick = 0;             // world tick when solved (flow delays from it)
    bool operator==(const PuzzleState &) const = default;
};

// Undo history of whole-World snapshots (World resource; never snapshotted itself).
struct UndoStack {
    lam::History history{8u << 20, 1024};
};

// Inserts Board/PuzzleState/UndoStack (and TileWorld/GridIndex when no plugin did), registers
// PuzzleState with snapshot_resource and installs the restore hook. Idempotent.
void install(lam::World &w);
// Replaces the puzzle entities with `level`'s start position. Presentation entities survive.
void load_level(lam::World &w, const Level &level, int index, lam::Vec2 origin = {});
// TileWorld from Board + PuzzleState::filled, then GridIndex::rebuild (also the restore hook).
void sync_tiles(lam::World &w);

// Decides legality with lam::can_step (the dry run of try_step; open water is the one Solid cell a
// stone may enter), pushes an undo snapshot, then moves the stone and the player with try_step.
// Nothing changes when the move is illegal (result.moved false).
MoveResult step(lam::World &w, Dir d);
bool undo(lam::World &w);                      // false if there is nothing to undo
// Back to the start position. keep_history: the restart itself can be undone (R); otherwise the
// history and the solved/helped flags are cleared too ("play it again").
void restart(lam::World &w, bool keep_history);

// Readers (bots, tests, presentation).
State read_state(lam::World &w);               // the ECS state as the pure rules' State
bool busy(lam::World &w);                      // any GridMover still gliding
lam::Entity player(lam::World &w);             // null if no level is loaded
lam::Entity stone_at(lam::World &w, lam::IVec2 cell);   // a stone that blocks (not sunk), or null
lam::Vec2 cell_center(const lam::World &w, lam::IVec2 cell);   // world pixels

}  // namespace pathar

// Reflection: tags and the counters are kSerialize|kSnapshot|kHash, so undo, saves and
// --hash-every see them. Dir is an enum field, reflected by name.
LAM_REFLECT_ENUM(pathar::Dir, "up", "down", "left", "right")
LAM_REFLECT_TAG(pathar::Player)
LAM_REFLECT_TAG(pathar::Sunk)
LAM_REFLECT(pathar::Stone, index)
LAM_REFLECT(pathar::Facing, dir)
LAM_REFLECT(pathar::PuzzleState, level, moves, pushes, filled, solved, helped, stuck_said, queued, path, path_len,
            path_pos, solved_tick)
