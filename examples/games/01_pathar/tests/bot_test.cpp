// App-level checks: the SolverBot plays through levels by pressing buttons (InputDriver), the
// glides run on tick time (grid_movers), runs are deterministic, and progress survives SaveSlots.
// They run a headless App with the engine's input, tilemap and scene modules.
#include <cstdint>
#include <span>
#include <string>
#include <doctest/doctest.h>
#include <lam/core/default_plugins.h>
#include <lam/input/driver.h>
#include <lam/physics/tile_world.h>
#include <lam/scene/save.h>
#include <lam/test/support.h>
#include "sim/board.h"
#include "sim/bot.h"
#include "sim/sim_plugin.h"
#include "test_util.h"

using namespace pathar;

namespace {

struct BotRun {
    int solved = 0;
    std::uint64_t ticks = 0;
    std::uint64_t hash = 0;
};

// Headless App + engine plugins + Pathar's sim plugin; the bot solves `levels` levels in order on
// the main world, reloading the next one whenever a level is solved.
BotRun run_bot(int levels, int max_ticks, std::uint64_t seed) {
    lam::test::HeadlessApp h(lam::AppConfig{.seed = seed});
    lam::App &app = h.app();
    app.add_plugins(lam::DefaultPlugins{.scenes = false});
    app.add_plugin<PatharSimPlugin>();
    SolverBot &bot = app.insert_resource<SolverBot>(app.res<Controls>());
    app.res<lam::InputSystem>().set_driver(&bot);
    const Catalogue &cat = test::catalogue();
    lam::World &w = app.main_world();
    auto load = [&](int index) {
        load_level(w, cat.at(index).level, index);
        w.resources().get<Board>().quiet = true;  // no sound bank here (the look plugin builds it)
    };
    load(0);
    BotRun run;
    for (int t = 0; t < max_ticks && run.solved < levels; ++t) {
        h.ticks(1);
        const PuzzleState &ps = w.resources().get<PuzzleState>();
        if (!ps.solved || busy(w)) continue;
        CHECK(ps.moves == cat.at(run.solved).level.par);  // the bot plays the solver's par line
        CHECK(ps.pushes == cat.at(run.solved).pushes);
        CHECK_FALSE(ps.helped);
        ++run.solved;
        if (run.solved < levels) load(run.solved);
    }
    run.ticks = app.tick();
    run.hash = w.state_hash();
    return run;
}

}  // namespace

TEST_CASE("pathar bot: solves all 24 levels in order at par by pressing buttons") {
    // par is the solver's move count (rules_test checks solve() against its recorded answers).
    const BotRun run = run_bot(24, 60 * 300, 1);
    CHECK(run.solved == 24);
}

TEST_CASE("pathar bot: the same seed gives the same run") {
    const BotRun a = run_bot(3, 60 * 30, 7);
    const BotRun b = run_bot(3, 60 * 30, 7);
    CHECK(a.solved == 3);
    CHECK(a.ticks == b.ticks);
    CHECK(a.hash == b.hash);
}

namespace {

// Presses one action on the next tick, as a key going down would.
struct Presser final : lam::InputDriver {
    lam::ButtonAction next;
    void fill(const lam::Ctx &, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override {
        if (next) frames[0].press(next);
        next = {};
    }
};

}  // namespace

TEST_CASE("pathar app: undo, restart and undoing the restart through the controls") {
    Presser presser;  // declared first: outlives the App that borrows it
    lam::test::HeadlessApp h;
    lam::App &app = h.app();
    app.add_plugins(lam::DefaultPlugins{.scenes = false});
    app.add_plugin<PatharSimPlugin>();
    app.res<lam::InputSystem>().set_driver(&presser);
    const Controls c = app.res<Controls>();
    lam::World &w = app.main_world();
    lam::Result<Level> parsed = Level::parse({"########", "#@$~ $.#", "########"}, "test");
    REQUIRE(parsed.ok());
    const Level l = parsed.value();
    load_level(w, l, 0);
    w.resources().get<Board>().quiet = true;
    const PuzzleState &ps = w.resources().get<PuzzleState>();
    const lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    const lam::IVec2 water = l.pos(l.water[0]);
    auto press = [&](lam::ButtonAction a) {  // one press, then let the glide finish
        presser.next = a;
        h.ticks(1);
        for (int i = 0; i < 4 * kGlideTicks && busy(w); ++i) h.ticks(1);
    };
    press(c.right);  // the stone sinks
    CHECK(ps.filled == 1u);
    CHECK(read_state(w).boxes[0] == -1);
    CHECK(tiles.at(water) == lam::TileShape::Empty);
    press(c.right);  // over the filled water
    CHECK(read_state(w).player == l.cell(3, 1));
    const State two = read_state(w);
    press(c.undo);
    CHECK(read_state(w).player == l.cell(2, 1));
    CHECK(ps.moves == 1);
    press(c.undo);
    CHECK(read_state(w) == start_state(l));
    CHECK(ps.moves == 0);
    CHECK(ps.filled == 0u);
    CHECK(tiles.at(water) == lam::TileShape::Solid);  // the restore hook re-derived the TileWorld
    press(c.right);
    press(c.right);
    CHECK(read_state(w) == two);
    press(c.restart);
    CHECK(read_state(w) == start_state(l));
    CHECK(ps.moves == 0);
    press(c.undo);  // R can be undone
    CHECK(read_state(w) == two);
    CHECK(ps.moves == 2);
    CHECK(ps.filled == 1u);
    press(c.left);  // walk back over the filled river
    press(c.left);
    CHECK(read_state(w).player == l.cell(1, 1));
    app.res<lam::InputSystem>().set_driver(nullptr);
}

TEST_CASE("pathar progress: SaveSlots round trip") {
    const Catalogue &cat = test::catalogue();
    lam::test::TempDir dir;
    Progress p(cat.total());
    p.finish(0, 3, 3);
    p.finish(1, 30, 19, true);
    p.last_played = 1;
    lam::SaveSlots slots(dir.path(), "pathar", 1, 1);
    REQUIRE(slots.save(1, lam::SaveEnvelope{.game = "pathar", .version = 1, .summary_json = R"({"stars": 4})",
                                            .data_json = p.to_json(cat)})
                .ok());
    CHECK(slots.exists(1));
    lam::Result<lam::SaveEnvelope> back = slots.load(1);
    REQUIRE(back.ok());
    const Progress q = Progress::from_json(back->data_json, cat);
    CHECK(q.total_stars() == 4);
    CHECK(q.record(1).best_moves == 0);  // solved with hints: no best of its own
    CHECK(q.last_played == 1);
    CHECK_FALSE(slots.save(2, lam::SaveEnvelope{}).ok());  // slot out of range
}
