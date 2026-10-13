// The ECS board on a bare World (no App): step() through try_step/GridIndex agrees with the pure
// kernel move for move on every shipped level; undo, restart and save-loading round-trip exactly
// (World::state_hash). Uses lam_tilemap (try_step, can_step, GridIndex, grid_movers) and lam_scene
// (serialize_world) on a bare World.
#include <string>
#include <doctest/doctest.h>
#include <lam/core/data.h>
#include <lam/ecs/world.h>
#include <lam/physics/tile_world.h>
#include <lam/scene/save.h>
#include <lam/tilemap/grid_mover.h>
#include "sim/board.h"
#include "test_util.h"

using namespace pathar;

namespace {

// Plays every glide to its end. A bare World has no systems, so this calls the engine's
// grid_movers body (TilemapPlugin runs it at Animate -1200) as the ticks would; try_step refuses
// (Busy) to move an entity whose GridMover is still moving.
void settle(lam::World &w) {
    for (int t = 0; t < 4 * kGlideTicks && busy(w); ++t) lam::grid_movers(w);
    REQUIRE_FALSE(busy(w));
}

Level parse(const std::vector<std::string> &rows) {
    lam::Result<Level> l = Level::parse(rows, "test");
    REQUIRE(l.ok());
    return l.value();
}

}  // namespace

// ---- History, snapshots, reflection ---------------------------------------------------------------

TEST_CASE("pathar board: History restores PuzzleState, tags and the derived TileWorld") {
    lam::World w;
    const Level l = parse({"########", "#@$~ $.#", "########"});
    load_level(w, l, 3);
    const lam::IVec2 water = l.pos(l.water[0]);
    const lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    CHECK(tiles.at(water) == lam::TileShape::Solid);
    CHECK(tiles.at(l.pos(l.player)) == lam::TileShape::Empty);
    CHECK(tiles.at({0, 1}) == lam::TileShape::Solid);
    const std::uint64_t start = w.state_hash();
    w.resources().get<UndoStack>().history.push(w);
    // What a sinking push leaves behind, written by hand (the step() cases below do it for real).
    PuzzleState &ps = w.resources().get<PuzzleState>();
    ps.moves = 4;
    ps.pushes = 1;
    ps.filled = 1;
    ps.path[5] = 3;
    sync_tiles(w);
    CHECK(tiles.at(water) == lam::TileShape::Empty);
    lam::Entity stone;
    for (auto [e, s] : w.view<Stone>()) {  // the stone beside the water (Level::boxes[0]);
        if (s.index == 0) stone = e;       // view order is not spawn order
    }
    REQUIRE(stone);
    w.add<Sunk>(stone);
    w.get<lam::GridPosition>(stone).blocks = false;
    CHECK(read_state(w).boxes[0] == -1);
    CHECK(w.state_hash() != start);
    REQUIRE(undo(w));
    CHECK(ps.moves == 0);  // resources never move: the reference sees the restored value
    CHECK(ps.filled == 0u);
    CHECK(ps.path[5] == 0);
    CHECK(tiles.at(water) == lam::TileShape::Solid);  // the restore hook ran sync_tiles
    CHECK_FALSE(w.has<Sunk>(stone));
    CHECK(w.get<lam::GridPosition>(stone).blocks);
    CHECK(read_state(w) == start_state(l));
    CHECK(w.state_hash() == start);
}

TEST_CASE("pathar board: PuzzleState round-trips through its reflection") {
    const lam::TypeLayout &layout = lam::Reflect<PuzzleState>::layout();
    CHECK(layout.fields.size() == 12);
    PuzzleState a{.level = 7, .moves = 31, .pushes = 9, .filled = 0x8000000000000001ull, .solved = true,
                  .helped = true, .stuck_said = true, .queued = 3};
    a.path[0] = 2;
    a.path[kMaxPath - 1] = 1;
    a.path_len = 40;
    a.path_pos = 12;
    a.solved_tick = 123456;
    lam::DataWriter out;
    out.begin_object();
    lam::write_fields(layout, &a, out, {});
    out.end_object();
    lam::Result<std::string> text = out.finish();
    REQUIRE(text.ok());
    lam::Result<lam::DataDocument> doc = lam::DataDocument::parse(text.value(), "state.json");
    REQUIRE(doc.ok());
    lam::DataReader in = doc->root();
    PuzzleState b;
    CHECK(lam::read_fields(layout, &b, in, {}).ok());
    CHECK(doc->finish().ok());
    CHECK(b == a);
}

TEST_CASE("pathar board: load_level replaces the pieces and keeps other entities") {
    lam::World w;
    const lam::Entity keep = w.create();
    load_level(w, parse({"#######", "#@ $ .#", "#######"}), 0);
    CHECK(w.count<Stone>() == 1);
    CHECK(w.count<Player>() == 1);
    const std::uint32_t serial = w.resources().get<Board>().serial;
    load_level(w, parse({"########", "#@$ $..#", "########"}), 1);
    CHECK(w.count<Stone>() == 2);
    CHECK(w.count<Player>() == 1);
    CHECK(w.alive(keep));
    CHECK(w.resources().get<Board>().serial == serial + 1);
    CHECK(w.resources().get<PuzzleState>().level == 1);
    CHECK(read_state(w) == start_state(w.resources().get<Board>().level));
}

// ---- try_step, GridIndex, serialize_world ----------------------------------------------------------

TEST_CASE("pathar board: step() agrees with the pure kernel on every level") {
    const Catalogue &cat = test::catalogue();
    for (int g = 0; g < cat.total(); ++g) {
        const Level &l = cat.at(g).level;
        CAPTURE(l.name);
        lam::World w;
        load_level(w, l, g);
        State kernel = start_state(l);
        CHECK(read_state(w) == kernel);
        const Solution sol = solve(l, kernel, 1000000);
        REQUIRE(sol.found);
        for (Dir d : sol.moves) {
            const MoveResult want = apply_move(l, kernel, d);
            const MoveResult got = step(w, d);
            settle(w);
            CHECK(got.moved == want.moved);
            CHECK(got.pushed == want.pushed);
            CHECK(got.sank == want.sank);
            CHECK(got.onto_goal == want.onto_goal);
            REQUIRE(read_state(w) == kernel);
        }
        const PuzzleState &ps = w.resources().get<PuzzleState>();
        CHECK(ps.moves == l.par);
        CHECK(ps.pushes == cat.at(g).pushes);
        CHECK(solved(l, read_state(w)));
    }
}

TEST_CASE("pathar board: illegal moves change nothing and push no undo") {
    lam::World w;
    load_level(w, parse({"########", "#@$$ ..#", "########"}), 0);
    const std::uint64_t before = w.state_hash();
    CHECK_FALSE(step(w, Dir::Up).moved);     // wall
    CHECK_FALSE(step(w, Dir::Right).moved);  // two stones
    CHECK(w.state_hash() == before);
    CHECK_FALSE(w.resources().get<UndoStack>().history.can_undo());
}

TEST_CASE("pathar board: undo round-trips exactly, water and tiles included") {
    lam::World w;
    const Level l = parse({"########", "#@$~ $.#", "########"});
    load_level(w, l, 0);
    const std::uint64_t start = w.state_hash();
    const lam::IVec2 water = l.pos(l.water[0]);
    CHECK(w.resources().get<lam::TileWorld>().at(water) == lam::TileShape::Solid);
    const MoveResult r = step(w, Dir::Right);  // the stone sinks
    settle(w);
    CHECK(r.sank);
    CHECK(w.resources().get<PuzzleState>().filled == 1u);
    CHECK(w.resources().get<lam::TileWorld>().at(water) == lam::TileShape::Empty);
    CHECK(step(w, Dir::Right).moved);  // walk over it
    settle(w);
    const std::uint64_t later = w.state_hash();
    CHECK(undo(w));
    CHECK(undo(w));
    CHECK_FALSE(undo(w));
    CHECK(w.state_hash() == start);  // undo keeps the tick (RestoreOptions::tick = false)
    CHECK(read_state(w) == start_state(l));
    // The restore hook re-derived the TileWorld from PuzzleState::filled.
    CHECK(w.resources().get<lam::TileWorld>().at(water) == lam::TileShape::Solid);
    CHECK(w.resources().get<UndoStack>().history.redo(w));
    CHECK(w.resources().get<UndoStack>().history.redo(w));
    CHECK(w.state_hash() == later);
}

TEST_CASE("pathar board: restart is undoable, replay is not") {
    lam::World w;
    const Level l = parse({"#######", "#@ $ .#", "#######"});
    load_level(w, l, 0);
    step(w, Dir::Right);
    settle(w);
    step(w, Dir::Right);
    settle(w);
    const State moved = read_state(w);
    restart(w, true);
    CHECK(read_state(w) == start_state(l));
    CHECK(w.resources().get<PuzzleState>().moves == 0);
    CHECK(undo(w));
    CHECK(read_state(w) == moved);
    CHECK(w.resources().get<PuzzleState>().moves == 2);
    restart(w, false);
    CHECK_FALSE(undo(w));
}

TEST_CASE("pathar board: a saved world loads back bit-identically") {
    const Level &l = test::catalogue().at(8).level;  // Pehli Leher: water
    lam::World a;
    load_level(a, l, 8);
    const Solution sol = solve(l, start_state(l));
    REQUIRE(sol.found);
    for (std::size_t i = 0; i < sol.moves.size() / 2; ++i) {
        step(a, sol.moves[i]);
        settle(a);
    }
    lam::DataWriter out;
    REQUIRE(lam::serialize_world(a, out, {}).ok());
    lam::Result<std::string> text = out.finish();
    REQUIRE(text.ok());
    lam::World b;
    load_level(b, l, 8);  // Board is static level data: not saved, set up by the game
    lam::Result<lam::DataDocument> doc = lam::DataDocument::parse(text.value(), "save.json");
    REQUIRE(doc.ok());
    lam::DataReader root = doc->root();
    REQUIRE(lam::deserialize_world(b, root, {}).ok());
    CHECK(read_state(b) == read_state(a));
    CHECK(b.state_hash() == a.state_hash());
    CHECK(b.resources().get<lam::TileWorld>().at(l.pos(l.water[0])) ==
          a.resources().get<lam::TileWorld>().at(l.pos(l.water[0])));
}
