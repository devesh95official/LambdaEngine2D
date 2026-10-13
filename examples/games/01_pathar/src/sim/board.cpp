// The ECS board (see board.h).
#include "sim/board.h"

#include <lam/core/log.h>
#include <lam/ecs/transform.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_mover.h>

namespace pathar {

namespace {

using lam::Entity;
using lam::IVec2;
using lam::World;

Board &board(World &w) { return w.resources().get<Board>(); }
PuzzleState &state(World &w) { return w.resources().get<PuzzleState>(); }

// Restore hook: run after History::undo/redo and deserialize_world (World::add_restore_hook).
void on_restore(World &w) { sync_tiles(w); }

// One TileWorld cell. try_step walks only on Empty cells with a non-zero path cost, and
// TileWorld::set keeps the cost, so shape and cost are always written together.
void set_cell(lam::TileWorld &tiles, IVec2 p, bool floor, bool open_water) {
    tiles.set(p, floor ? lam::TileShape::Empty : lam::TileShape::Solid,
              open_water ? static_cast<std::uint8_t>(lam::kTileWater) : std::uint8_t{0});
    tiles.cost(p.x, p.y) = floor ? std::uint16_t{10} : std::uint16_t{0};
}

Entity spawn_piece(World &w, IVec2 cell) {
    const Entity e = w.create();
    w.add<lam::Transform>(e, lam::Transform{.position = cell_center(w, cell)});
    w.add<lam::GridPosition>(e, lam::GridPosition{.cell = cell});
    w.add<lam::GridMover>(e, lam::GridMover{.from = cell, .to = cell, .ticks = kGlideTicks});
    return e;
}

// Puts every piece back on its start cell (teleport on the grid: no glide).
void reset_pieces(World &w) {
    const Level &l = board(w).level;
    std::vector<Entity> sunk;
    for (auto [e, stone, gp] : w.view<Stone, lam::GridPosition>()) {
        (void)gp;
        if (w.has<Sunk>(e)) sunk.push_back(e);
    }
    for (Entity e : sunk) w.remove<Sunk>(e);  // structural change: outside the view
    std::vector<std::pair<Entity, IVec2>> moves;
    for (auto [e, stone] : w.view<Stone>()) moves.push_back({e, l.pos(l.boxes[stone.index])});
    for (auto &[e, cell] : moves) {
        w.get<lam::GridPosition>(e).blocks = true;
        lam::set_grid_position(w, e, cell, true);
    }
    const Entity hero = player(w);
    if (hero) {
        lam::set_grid_position(w, hero, l.pos(l.player), true);
        w.get<Facing>(hero).dir = Dir::Down;
    }
}

}  // namespace

void install(World &w) {
    lam::Resources &r = w.resources();
    if (!r.has<Board>()) r.insert<Board>();
    if (!r.has<PuzzleState>()) r.insert<PuzzleState>();
    if (!r.has<UndoStack>()) r.insert<UndoStack>();
    // Normally PhysicsPlugin/TilemapPlugin insert these in init_world; bare test worlds do not.
    lam::install_grid_resources(w);
    w.snapshot_resource<PuzzleState>();  // idempotent
    if (!w.has_restore_hook(&on_restore)) w.add_restore_hook(&on_restore);  // hooks are not idempotent
}

void load_level(World &w, const Level &level, int index, lam::Vec2 origin) {
    install(w);
    std::vector<Entity> old;
    for (auto [e, gp] : w.view<lam::GridPosition>()) {
        (void)gp;
        if (w.has<Stone>(e) || w.has<Player>(e)) old.push_back(e);
    }
    for (Entity e : old) w.destroy(e);
    Board &b = board(w);
    b.level = level;
    b.dead = dead_squares(level);
    b.index = index;
    b.origin = origin;
    ++b.serial;
    state(w) = PuzzleState{.level = index};
    w.resources().get<UndoStack>().history.clear();
    lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    tiles.resize(level.width, level.height, kTileSize, origin);  // cell_center needs the origin
    const Entity hero = spawn_piece(w, level.pos(level.player));
    w.add<Player>(hero);
    w.add<Facing>(hero);
    for (std::size_t i = 0; i < level.boxes.size(); ++i) {
        const Entity s = spawn_piece(w, level.pos(level.boxes[i]));
        w.add<Stone>(s, Stone{static_cast<std::uint8_t>(i)});
    }
    sync_tiles(w);
}

void sync_tiles(World &w) {
    const Board &b = board(w);
    const PuzzleState &ps = state(w);
    const Level &l = b.level;
    lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    if (tiles.shapes.width() != l.width || tiles.shapes.height() != l.height || tiles.origin != b.origin)
        tiles.resize(l.width, l.height, kTileSize, b.origin);
    tiles.outside_solid = true;
    for (int c = 0; c < l.width * l.height; ++c) {
        const IVec2 p = l.pos(c);
        const int wi = l.water_index(c);
        const bool open = wi >= 0 && ((ps.filled >> wi) & 1u) == 0;
        const bool floor = l.tile(c) == Tile::Ground || (l.tile(c) == Tile::Water && !open);
        set_cell(tiles, p, floor, open);
    }
    w.resources().get<lam::GridIndex>().rebuild(w, tiles);
}

MoveResult step(World &w, Dir d) {
    MoveResult r;
    const Entity hero = player(w);
    if (!hero) return r;
    Board &b = board(w);
    PuzzleState &ps = state(w);
    const Level &l = b.level;
    lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    const IVec2 dv = dir_vec(d);
    // 1. Legality, read-only, with lam::can_step (the dry run of try_step): walls and open water
    //    are Solid in the TileWorld, cells outside it are Solid, and a stone blocks the player.
    Entity stone;
    const lam::StepResult hero_step = lam::can_step(w, hero, dv, {}, &stone);
    if (hero_step == lam::StepResult::BlockedEntity) {
        if (!w.has<Stone>(stone) || w.has<Sunk>(stone)) return r;
    } else if (hero_step != lam::StepResult::Moved) {
        return r;  // a wall, open water, or a glide still under way (Busy)
    } else {
        stone = {};
    }
    int water = -1;
    if (stone) {
        const IVec2 from = w.get<lam::GridPosition>(stone).cell;
        const IVec2 beyond = from + dv;
        const lam::StepResult stone_step = lam::can_step(w, stone, dv);
        if (stone_step == lam::StepResult::BlockedTile && l.inside(beyond)) {
            // Open water is Solid, but a stone may enter it: it sinks and fills the cell.
            const int wi = l.water_index(l.cell(beyond));
            if (wi < 0 || ((ps.filled >> wi) & 1u) != 0) return r;
            water = wi;
        } else if (stone_step != lam::StepResult::Moved) {
            return r;  // a wall, another stone, or outside the board
        }
        r.pushed = true;
        r.box = w.get<Stone>(stone).index;
        r.from = l.cell(from);
        r.to = l.cell(beyond);
        r.off_goal = l.is_goal(r.from);
        r.sank = water >= 0;
        r.onto_goal = !r.sank && l.is_goal(r.to);
    }
    // 2. Remember the position (whole-World snapshot) before anything changes.
    w.resources().get<UndoStack>().history.push(w);
    // 3. Act: the stone first, so the player's target cell is free in the GridIndex.
    if (stone) {
        if (water >= 0) {
            ps.filled |= std::uint64_t{1} << water;
            set_cell(tiles, l.pos(r.to), true, false);  // filled: floor, so the stone may glide in
        }
        if (lam::try_step(w, stone, dv) != lam::StepResult::Moved) LAM_LOG_WARN("pathar: stone step refused");
        if (water >= 0) {
            w.get<lam::GridPosition>(stone).blocks = false;  // walkable from now on
            w.add<Sunk>(stone);
        }
        ++ps.pushes;
    }
    if (lam::try_step(w, hero, dv) != lam::StepResult::Moved) LAM_LOG_WARN("pathar: player step refused");
    w.get<Facing>(hero).dir = d;
    ++ps.moves;
    r.moved = true;
    return r;
}

bool undo(World &w) {
    // History::undo restores every snapshotted pool and PuzzleState, then runs on_restore.
    if (!w.resources().get<UndoStack>().history.undo(w)) return false;
    PuzzleState &ps = state(w);
    ps.queued = 0;
    ps.path_len = ps.path_pos = 0;
    return true;
}

void restart(World &w, bool keep_history) {
    UndoStack &u = w.resources().get<UndoStack>();
    if (keep_history) u.history.push(w);
    else u.history.clear();
    PuzzleState &ps = state(w);
    const PuzzleState old = ps;
    ps = PuzzleState{.level = old.level};
    if (keep_history) ps.helped = old.helped;  // a hint already used still counts
    reset_pieces(w);
    sync_tiles(w);
}

State read_state(World &w) {
    State s;
    const Board &b = board(w);
    const Level &l = b.level;
    s.boxes.assign(l.boxes.size(), -1);
    s.filled = state(w).filled;
    for (auto [e, gp] : w.view<lam::GridPosition>()) {
        const int c = l.inside(gp.cell) ? l.cell(gp.cell) : -1;
        if (w.has<Player>(e)) s.player = c;
        if (const Stone *st = w.try_get<Stone>(e); st && !w.has<Sunk>(e)) s.boxes[st->index] = c;
    }
    return s;
}

bool busy(World &w) {
    for (auto [e, m] : w.view<lam::GridMover>()) {
        (void)e;
        if (m.moving) return true;
    }
    return false;
}

Entity player(World &w) {
    Entity e;
    w.single_or_null<Player>(&e);
    return e;
}

Entity stone_at(World &w, IVec2 cell) {
    for (Entity e : w.resources().get<lam::GridIndex>().at(cell)) {
        if (w.has<Stone>(e) && !w.has<Sunk>(e)) return e;
    }
    return {};
}

lam::Vec2 cell_center(const World &w, IVec2 cell) {
    return w.resources().get<lam::TileWorld>().tile_center(cell);
}

}  // namespace pathar
