#pragma once
// Sokoban-style rules. Section 1 is plain data and functions (used by the test and the bot);
// section 2 is the ECS game, which asks section 1 before it changes anything.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/core/result.h>
#include <lam/ecs/component.h>
#include <lam/ecs/snapshot.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/math/grid.h>
#include <lam/math/vec2.h>

namespace lam {
struct EmitterConfig;
}

namespace grid_puzzle {

inline constexpr int kTile = 16;                                // pixels per cell
inline constexpr std::uint16_t kStepTicks = 6;                  // one step glides over 6 ticks (0.1 s)
inline constexpr std::array<lam::IVec2, 4> kDirs{{{0, -1}, {1, 0}, {0, 1}, {-1, 0}}};   // up right down left

// ---- 1. Pure rules ----------------------------------------------------------------------------
struct State {                                                  // where the movable things are
    lam::IVec2 player;
    std::vector<lam::IVec2> crates;
    bool operator==(const State &) const = default;
};
struct Level {                                                  // never changes during play
    lam::Grid<char> cells;                                      // '#' wall, '.' goal, ' ' floor
    State start;
    bool wall(lam::IVec2 c) const { return cells.get_or(c, '#') == '#'; }   // off the board = wall
    bool goal(lam::IVec2 c) const { return cells.get_or(c, ' ') == '.'; }
};
// Standard Sokoban text (# @ $ . * + and space). A blank line ends a level; ';' starts a comment.
lam::Result<std::vector<Level>> parse_levels(std::string_view text);

enum class Move : std::uint8_t { Blocked, Walk, Push };
// TODO(rules): your mechanics go here (ice, keys, switches...). Changes `s` only when legal.
Move try_move(const Level &level, State &s, lam::IVec2 dir);
bool solved(const Level &level, const State &s);                // every crate is on a goal
// Breadth-first search: the shortest list of directions that solves `level` from `from`, or an
// empty list. Fine for small levels; a real solver needs pruning (dead squares, push-only search).
std::vector<lam::IVec2> solve(const Level &level, const State &from, std::size_t max_states = 100000);

// ---- 2. The ECS game ----------------------------------------------------------------------------
struct Player {};                                               // tags on the piece entities
struct Crate {};
// Rule state besides the entities. Snapshotted, so undo, saves and the state hash cover it.
struct Run {
    std::int32_t level = 0;                                     // index into Levels::list
    std::int32_t moves = 0, pushes = 0;
    std::int32_t cleared = 0;                                   // levels solved this session
    std::uint8_t queued = 0;                                    // a step pressed mid-glide: 1 + kDirs index
    bool solved = false;
};
struct Levels { std::vector<Level> list; };                     // App resource (main.cpp reads levels.txt)
struct Undo { lam::History history{4u << 20, 512}; };          // World resource; never snapshotted itself
struct Fx { lam::AssetId<lam::EmitterConfig> dust; };          // App resource; absent (tests) = no particles
struct Controls {                                               // App resource: action ids resolved once
    lam::ButtonAction up, right, down, left, undo, restart, next;
    std::array<lam::ButtonAction, 4> dirs() const { return {up, right, down, left}; }   // kDirs order
};

// Puts `level` into `w`: walls into the TileWorld, fresh pieces, a fresh Run (keeps `cleared`) and
// an empty undo history. Call it from a startup or tick system, never inside a view.
void load_level(lam::World &w, const Level &level, std::int32_t index);
State read_state(lam::World &w);                                // the pieces' cells, as layer 1 sees them
bool busy(lam::World &w);                                       // a piece is still gliding

class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "grid_puzzle.rules"; }
    void build(lam::App &app) override;                         // components, events, controls, systems
};

// --bot: solves the focused world's level and presses the buttons a player would.
class SolverBot final : public lam::InputDriver {
public:
    explicit SolverBot(Controls controls) : controls_(controls) {}
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
private:
    Controls controls_;
    std::vector<lam::IVec2> plan_;
    std::size_t next_ = 0;
    State expect_;                                              // where the last press should lead
    std::int32_t level_ = -1;                                   // the level the plan belongs to
};

}  // namespace grid_puzzle

// Reflection has to be at global scope.
LAM_REFLECT_TAG(grid_puzzle::Player)
LAM_REFLECT_TAG(grid_puzzle::Crate)
LAM_REFLECT(grid_puzzle::Run, level, moves, pushes, cleared, queued, solved)
