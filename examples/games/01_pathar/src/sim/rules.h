#pragma once
// Pathar's rules as pure data (no ECS, no engine services): the level grid, a compact State, the
// one-move kernel, dead squares, stars and the breadth-first solver.
//
// The solver explores hundreds of thousands of positions, so it needs a cheap value type rather
// than a World. tests/world_test.cpp checks that this kernel and the ECS board (board.h) agree
// move for move.
//
// Levels are rows of characters (Sokoban letters plus water):
//   #  wall          .  marked slab        $  stone        *  stone on a slab
//   @  player        +  player on a slab   ~  water        (space) ground
//   =  a bridge over the water (ground; only its look differs)
// Ground the player cannot reach from the start is outside the level (not drawn).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <lam/core/result.h>
#include <lam/math/vec2.h>

namespace pathar {

enum class Tile : std::uint8_t { Outside, Wall, Ground, Water };
enum class Dir : std::uint8_t { Up, Down, Left, Right };
inline constexpr Dir kDirs[4] = {Dir::Up, Dir::Down, Dir::Left, Dir::Right};

lam::IVec2 dir_vec(Dir d);  // Up = (0, -1): +y is down (engine convention)
Dir opposite(Dir d);

struct Level {
    std::string name;
    int width = 0;
    int height = 0;
    std::vector<Tile> tiles;
    std::vector<std::uint8_t> goal;    // per cell
    std::vector<std::uint8_t> bridge;  // per cell: ground drawn as a bridge
    std::vector<int> boxes;            // start cells, one per stone (stone index = position here)
    std::vector<int> water;            // the water cells, in order (their bit in State::filled)
    int player = 0;
    int par = 0;                       // moves of the solver's solution (0 = unknown)

    // Error{Parse} with the level name when the rows are not a valid level.
    static lam::Result<Level> parse(const std::vector<std::string> &rows, std::string name);
    int cell(int x, int y) const { return y * width + x; }
    int cell(lam::IVec2 p) const { return cell(p.x, p.y); }
    lam::IVec2 pos(int c) const { return {c % width, c / width}; }
    bool inside(lam::IVec2 p) const { return p.x >= 0 && p.y >= 0 && p.x < width && p.y < height; }
    int step(int cell, Dir d) const;  // the neighbour, -1 outside the grid
    int goal_count() const;
    int water_index(int cell) const;  // -1 if not water
    Tile tile(int c) const { return tiles[static_cast<std::size_t>(c)]; }
    bool is_goal(int c) const { return goal[static_cast<std::size_t>(c)] != 0; }
};

// One position: the player, every stone (-1 = sunk) and the filled water bits.
struct State {
    int player = 0;
    std::vector<int> boxes;
    std::uint64_t filled = 0;  // bit i = Level::water[i] holds a sunk stone
    bool operator==(const State &) const = default;
};

struct MoveResult {
    bool moved = false;
    bool pushed = false;
    bool sank = false;    // the stone went into the water
    int box = -1;         // which stone
    int from = -1;        // the stone's cell before
    int to = -1;          // after (the water's cell when it sank)
    bool onto_goal = false;
    bool off_goal = false;
};

State start_state(const Level &level);
int box_at(const State &s, int cell);  // stone index or -1
bool open_water(const Level &level, const State &s, int cell);
bool floor_at(const Level &level, const State &s, int cell);  // ground or filled water
int on_goals(const Level &level, const State &s);
bool solved(const Level &level, const State &s);
// A stone off its slab that can never reach one (dead square), or too few stones left.
bool stuck(const Level &level, const State &s, const std::vector<std::uint8_t> &dead);
// The kernel: one step of the player in `d`, pushing at most one stone. Changes `s` only if moved.
MoveResult apply_move(const Level &level, State &s, Dir d);

// Squares from which a stone can never be pushed onto a slab or into water (ignoring other stones).
std::vector<std::uint8_t> dead_squares(const Level &level);

// 3 stars at par or better, 2 a little over, 1 for solving at all.
int stars(int moves, int par);

struct Solution {
    bool found = false;
    bool gave_up = false;    // too many states
    std::vector<Dir> moves;
    int pushes = 0;
    std::size_t states = 0;  // push positions looked at
};
// Fewest pushes from `from` (then the shortest walks between them). Gives up after max_states.
Solution solve(const Level &level, const State &from, std::size_t max_states = 400000);

// The walk (no pushing) from the player to `target`; empty if unreachable or already there.
std::vector<Dir> walk_path(const Level &level, const State &s, int target);

}  // namespace pathar
