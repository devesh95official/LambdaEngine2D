// Grid movement: GridIndex, try_step, the grid_movers tween and TurnScheduler::busy.
#include <cstdint>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/turns.h>
#include <lam/ecs/world.h>
#include <lam/math/ease.h>
#include <lam/physics/physics_plugin.h>
#include <lam/test/support.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/tilemap/tilemap_plugin.h>

using namespace lam;

struct Crate {};
LAM_REFLECT_TAG(Crate)

namespace {

// A bare World with a TileWorld from rows ('#' solid, 'x' impassable floor, else free) and a
// GridIndex, as TilemapPlugin::init_world sets them up.
void setup(World &w, std::initializer_list<const char *> rows, int tile = 16) {
    const int width = static_cast<int>(std::string_view(*rows.begin()).size());
    TileWorld &tw = w.resources().insert<TileWorld>();
    tw.resize(width, static_cast<int>(rows.size()), tile, {0, 0});
    int y = 0;
    for (const char *row : rows) {
        for (int x = 0; x < width; ++x) {
            if (row[x] == '#') {
                tw.set({x, y}, TileShape::Solid, kTileOpaque);
                tw.cost(x, y) = 0;
            } else if (row[x] == 'x') {
                tw.cost(x, y) = 0;
            }
        }
        ++y;
    }
    w.resources().insert<GridIndex>();
}

Entity spawn_at(World &w, IVec2 cell, bool mover = false, bool blocks = true,
                CollisionMask layer = 1) {
    const Entity e = w.create();
    w.add<Transform>(e, Transform{.position = w.resources().get<TileWorld>().tile_center(cell)});
    w.add<GridPosition>(e, GridPosition{cell, layer, blocks});
    if (mover)
        w.add<GridMover>(e, GridMover{.from = cell, .to = cell, .ticks = 4, .ease = Ease::Linear});
    return e;
}

void rebuild(World &w) {
    grid_index_rebuild(w);
}

} // namespace

TEST_CASE("grid index: who stands where, sorted by entity index, kept current by updates") {
    World w;
    setup(w, {"....", "....", "...."});
    const Entity a = spawn_at(w, {1, 1});
    const Entity b = spawn_at(w, {1, 1}, false, false);
    const Entity c = spawn_at(w, {3, 2});
    const Entity off = spawn_at(w, {9, 9}); // outside the grid: not indexed
    rebuild(w);
    const GridIndex &index = w.resources().get<GridIndex>();
    CHECK(index.size() == IVec2{4, 3});
    REQUIRE(index.at({1, 1}).size() == 2);
    CHECK(index.at({1, 1})[0] == a);
    CHECK(index.at({1, 1})[1] == b);
    CHECK(index.at({3, 2}).size() == 1);
    CHECK(index.at({0, 0}).empty());
    CHECK(index.at({-1, 0}).empty());
    CHECK(index.at({9, 9}).empty());
    (void)off;
    // update moves one entry and keeps runs sorted, growing a cell past its slack.
    GridIndex &mut = w.resources().get<GridIndex>();
    mut.update(c, {3, 2}, {1, 1});
    CHECK(index.at({3, 2}).empty());
    REQUIRE(index.at({1, 1}).size() == 3);
    CHECK(index.at({1, 1})[2] == c);
    std::vector<Entity> extra;
    for (int i = 0; i < 6; ++i)
        extra.push_back(spawn_at(w, {0, 0}));
    for (const Entity e : extra)
        mut.update(e, {0, 0}, {1, 1}); // never indexed at (0, 0): insert only
    REQUIRE(index.at({1, 1}).size() == 9);
    for (std::size_t i = 1; i < index.at({1, 1}).size(); ++i) {
        CHECK(index.at({1, 1})[i - 1].index < index.at({1, 1})[i].index);
    }
    mut.update(a, {1, 1}, {-4, 0}); // leaving the grid
    CHECK(index.at({1, 1}).size() == 8);
    // A rebuild agrees with the GridPositions, and is allocation-free once warmed up.
    w.get<GridPosition>(a).cell = {2, 0};
    rebuild(w);
    CHECK(index.at({2, 0}).size() == 1);
    CHECK(index.at({0, 0}).size() == 6);
    CHECK_NO_ALLOC({
        for (int i = 0; i < 20; ++i)
            rebuild(w);
    });
}

TEST_CASE("try_step: free cells move, walls and impassable floors block, the edge blocks") {
    World w;
    setup(w, {"#....", "#.x..", "#...."});
    const Entity e = spawn_at(w, {1, 0});
    rebuild(w);
    CHECK(try_step(w, e, {-1, 0}) == StepResult::BlockedTile); // wall
    CHECK(try_step(w, e, {0, -1}) == StepResult::BlockedTile); // outside the grid
    CHECK(try_step(w, e, {1, 0}) == StepResult::Moved);
    CHECK(w.get<GridPosition>(e).cell == IVec2{2, 0});
    CHECK(w.get<Transform>(e).position == Vec2{40, 8}); // no GridMover: jumps to the cell centre
    CHECK(w.resources().get<GridIndex>().at({2, 0}).size() == 1);
    CHECK(w.resources().get<GridIndex>().at({1, 0}).empty());
    CHECK(try_step(w, e, {0, 1}) == StepResult::BlockedTile); // cost 0 floor
    CHECK(try_step(w, e, {5, 0}) == StepResult::Moved);       // clamped to one cell
    CHECK(w.get<GridPosition>(e).cell == IVec2{3, 0});
    CHECK(try_step(w, e, {0, 0}) == StepResult::Busy);
    CHECK(try_step(w, e, {1, 1}) == StepResult::Busy); // diagonal not allowed by default
    StepRules diag;
    diag.diagonal = true;
    CHECK(try_step(w, e, {1, 1}, diag) == StepResult::Moved);
    CHECK(w.get<GridPosition>(e).cell == IVec2{4, 1});
    CHECK(try_step(w, e, {-1, 0}) == StepResult::Moved);
    CHECK(try_step(w, e, {-1, 1}, diag) ==
          StepResult::BlockedTile); // would cut the corner of (2, 1)
    CHECK(try_step(w, e, {-1, -1}, diag) == StepResult::BlockedTile); // likewise
    CHECK(try_step(w, e, {1, 1}, diag) == StepResult::Moved);
    CHECK(w.get<GridPosition>(e).cell == IVec2{4, 2});
}

TEST_CASE("try_step: blocking entities by layer, the lowest index reported, and a Sokoban push") {
    World w;
    setup(w, {".....", ".....", "....#"});
    const Entity hero = spawn_at(w, {0, 0});
    const Entity ghost = spawn_at(w, {1, 0}, false, false);    // does not block
    const Entity spirit = spawn_at(w, {0, 1}, false, true, 4); // another layer
    const Entity crate = spawn_at(w, {2, 0});
    const Entity rock = spawn_at(w, {2, 0}); // two blockers on one cell
    w.add<Crate>(crate);
    rebuild(w);
    Entity blocker;
    CHECK(try_step(w, hero, {1, 0}, {}, &blocker) == StepResult::Moved); // through the ghost
    CHECK(try_step(w, hero, {1, 0}, {}, &blocker) == StepResult::BlockedEntity);
    CHECK(blocker == crate); // the lower entity index of the two
    CHECK(w.get<GridPosition>(hero).cell == IVec2{1, 0});
    StepRules only_layer2;
    only_layer2.blocking = 2;
    CHECK(try_step(w, hero, {-1, 0}, only_layer2) == StepResult::Moved);
    CHECK(try_step(w, hero, {0, 1}, only_layer2) == StepResult::Moved); // onto the spirit (layer 4)
    CHECK(try_step(w, hero, {0, -1}) == StepResult::Moved);
    CHECK(try_step(w, hero, {0, 1}) ==
          StepResult::BlockedEntity); // layer 4 blocks with the default mask
    (void)ghost;
    (void)spirit;
    // Sokoban: remove the rock, then push the crate along the row until it reaches the edge.
    w.destroy(rock);
    rebuild(w);
    CHECK(try_step(w, hero, {1, 0}) == StepResult::Moved);
    const auto push = [&](IVec2 dir) {
        Entity b;
        StepResult r = try_step(w, hero, dir, {}, &b);
        if (r == StepResult::BlockedEntity && w.has<Crate>(b) &&
            try_step(w, b, dir) == StepResult::Moved) {
            r = try_step(w, hero, dir);
        }
        return r;
    };
    CHECK(push({1, 0}) == StepResult::Moved);
    CHECK(w.get<GridPosition>(crate).cell == IVec2{3, 0});
    CHECK(w.get<GridPosition>(hero).cell == IVec2{2, 0});
    CHECK(push({1, 0}) == StepResult::Moved);
    CHECK(push({1, 0}) == StepResult::BlockedEntity); // the crate is against the edge
    CHECK(w.get<GridPosition>(crate).cell == IVec2{4, 0});
    CHECK(w.resources().get<GridIndex>().at({4, 0}).size() == 1);
}

TEST_CASE("grid movers: steps tween over ticks, a moving entity is busy, set_grid_position") {
    World w;
    setup(w, {"......"});
    const Entity e = spawn_at(w, {0, 0}, true);
    rebuild(w);
    CHECK(try_step(w, e, {1, 0}) == StepResult::Moved);
    const GridMover &mv = w.get<GridMover>(e);
    CHECK(mv.moving);
    CHECK(mv.from == IVec2{0, 0});
    CHECK(mv.to == IVec2{1, 0});
    CHECK(w.get<Transform>(e).position == Vec2{8, 8}); // the tween has not started
    CHECK(try_step(w, e, {1, 0}) == StepResult::Busy);
    grid_movers(w);
    CHECK(w.get<Transform>(e).position.x == doctest::Approx(12.0f)); // 1/4 of 16 px, linear
    grid_movers(w);
    grid_movers(w);
    CHECK(w.get<GridMover>(e).moving);
    grid_movers(w);
    CHECK_FALSE(w.get<GridMover>(e).moving);
    CHECK(w.get<Transform>(e).position == Vec2{24, 8});
    CHECK(try_step(w, e, {1, 0}) == StepResult::Moved);
    // A teleport stops the tween and snaps Transform.
    set_grid_position(w, e, {5, 0});
    CHECK_FALSE(w.get<GridMover>(e).moving);
    CHECK(w.get<GridPosition>(e).cell == IVec2{5, 0});
    CHECK(w.get<Transform>(e).position == Vec2{88, 8});
    CHECK(w.resources().get<GridIndex>().at({5, 0}).size() == 1);
    CHECK(w.resources().get<GridIndex>().at({2, 0}).empty());
    // Without snap a mover glides from the old cell.
    set_grid_position(w, e, {3, 0}, false);
    CHECK(w.get<GridMover>(e).moving);
    CHECK(w.get<GridMover>(e).from == IVec2{5, 0});
    for (int i = 0; i < 4; ++i)
        grid_movers(w);
    CHECK(w.get<Transform>(e).position == Vec2{56, 8});
    // ticks = 0: the step lands at once on the next grid_movers.
    w.get<GridMover>(e).ticks = 0;
    CHECK(try_step(w, e, {-1, 0}) == StepResult::Moved);
    grid_movers(w);
    CHECK_FALSE(w.get<GridMover>(e).moving);
    CHECK(w.get<Transform>(e).position == Vec2{40, 8});
}

namespace {

int g_moves = 0;

// Simulate: the hero walks right whenever it is its turn and nothing animates.
void walk_right(Ctx &ctx) {
    World &w = ctx.world();
    if (const Entity e = next_actor(w)) {
        if (try_step(w, e, {1, 0}) == StepResult::Moved) ++g_moves;
        end_turn(w, e);
    }
}

void start_hero(Ctx &ctx) {
    World &w = ctx.world();
    TileWorld &tw = w.resources().get<TileWorld>();
    tw.resize(64, 3, 16, {0, 0});
    add_turn_scheduler(w);
    const Entity e = w.create();
    w.add<Transform>(e, Transform{.position = tw.tile_center({0, 1})});
    w.add<GridPosition>(e, GridPosition{{0, 1}});
    w.add<GridMover>(e, GridMover{.from = {0, 1}, .to = {0, 1}, .ticks = 3});
    w.add<TurnActor>(e, TurnActor{.energy = 0, .speed = 1000});
}

} // namespace

TEST_CASE("grid movers: TurnScheduler stays busy while a step animates, inside the App") {
    g_moves = 0;
    test::HeadlessApp h;
    h.add<AssetsPlugin>();
    h.add<PhysicsPlugin>();
    h.add<TilemapPlugin>();
    h.app().add_startup(&start_hero, "start_hero");
    h.app().add_system(Stage::Simulate, &walk_right, "walk_right");
    World &w = h.app().main_world();
    const auto busy = [&] { return w.resources().get<TurnScheduler>().busy; };
    h.ticks(1);
    CHECK(g_moves == 1);
    REQUIRE(w.resources().has<TurnScheduler>());
    CHECK(busy()); // grid_movers (Animate) raised it
    h.ticks(1);
    CHECK(g_moves == 1); // busy: no turn while the step animates
    CHECK(busy());
    h.ticks(1);
    CHECK(g_moves == 1);
    CHECK_FALSE(busy()); // the third tick ends the step
    h.ticks(1);
    CHECK(g_moves == 2);
    CHECK(w.get<GridPosition>(w.single<GridMover>()).cell == IVec2{2, 1});
    // A busy flag raised by the game is not lowered by grid_movers.
    for (int i = 0; i < 10 && busy(); ++i)
        h.ticks(1);
    REQUIRE_FALSE(busy());
    set_turns_busy(w, true);
    const int moves = g_moves;
    h.ticks(5);
    CHECK(busy());
    CHECK(g_moves == moves);
    set_turns_busy(w, false);
    // Steady state: walking and animating allocate nothing.
    h.ticks(8);
    CHECK_NO_ALLOC(h.ticks(40));
    CHECK(g_moves > 10);
}

// ---- can_step, install_grid_resources, update ---------------------------------------------------

TEST_CASE("can_step: a dry run answers what try_step would and changes nothing") {
    World w;
    setup(w, {"#....", "#.x..", "#...."});
    const Entity hero = spawn_at(w, {1, 0}, true);
    const Entity crate = spawn_at(w, {2, 0});
    rebuild(w);
    const std::uint64_t before = w.state_hash();
    Entity blocker;
    CHECK(can_step(w, hero, {-1, 0}) == StepResult::BlockedTile);
    CHECK(can_step(w, hero, {0, -1}) == StepResult::BlockedTile);
    CHECK(can_step(w, hero, {1, 0}, {}, &blocker) == StepResult::BlockedEntity);
    CHECK(blocker == crate);
    CHECK(can_step(w, hero, {1, 0}, StepRules{.blocking = 2}) == StepResult::Moved);
    CHECK(can_step(w, hero, {0, 1}) == StepResult::Moved);
    CHECK(can_step(w, hero, {1, 1}) == StepResult::Busy); // diagonal not allowed
    CHECK(can_step(w, hero, {0, 0}) == StepResult::Busy);
    CHECK(w.state_hash() == before);
    CHECK(w.get<GridPosition>(hero).cell == IVec2{1, 0});
    CHECK_FALSE(w.get<GridMover>(hero).moving);
    CHECK(w.resources().get<GridIndex>().at({1, 0}).size() == 1);
    CHECK(w.resources().get<GridIndex>().at({1, 1}).empty());
    // It agrees with try_step, including Busy while a step animates.
    CHECK(try_step(w, hero, {0, 1}) == StepResult::Moved);
    CHECK(can_step(w, hero, {0, 1}) == StepResult::Busy);
    CHECK(try_step(w, hero, {0, 1}) == StepResult::Busy);
    // A const World is enough, and nothing allocates.
    const World &cw = w;
    StepResult r = StepResult::Moved;
    CHECK_NO_ALLOC(r = can_step(cw, crate, {1, 0}));
    CHECK(r == StepResult::Moved);
}

TEST_CASE("install_grid_resources: grid movement on a bare World without plugins") {
    World w;
    CHECK_FALSE(w.resources().has<TileWorld>());
    install_grid_resources(w);
    REQUIRE(w.resources().has<TileWorld>());
    REQUIRE(w.resources().has<GridIndex>());
    TileWorld *tw = &w.resources().get<TileWorld>();
    install_grid_resources(w); // idempotent: the existing resources are kept
    CHECK(&w.resources().get<TileWorld>() == tw);
    tw->resize(4, 1, 16, {0, 0});
    tw->set({3, 0}, TileShape::Solid, 0);
    tw->cost(3, 0) = 0;
    const Entity e = w.create();
    w.add<Transform>(e);
    w.add<GridPosition>(e, GridPosition{{0, 0}});
    grid_index_rebuild(w);
    CHECK(try_step(w, e, {1, 0}) == StepResult::Moved);
    CHECK(try_step(w, e, {1, 0}) == StepResult::Moved);
    CHECK(try_step(w, e, {1, 0}) == StepResult::BlockedTile);
    CHECK(w.get<Transform>(e).position == Vec2{40, 8});
    // A TileWorld inserted before (by a game or PhysicsPlugin) is not replaced.
    World other;
    TileWorld &mine = other.resources().insert<TileWorld>();
    mine.resize(2, 2, 8, {0, 0});
    install_grid_resources(other);
    CHECK(other.resources().get<TileWorld>().tile_size == 8);
    CHECK(other.resources().has<GridIndex>());
}

TEST_CASE("grid index: update inserts an entity that was never indexed, once") {
    World w;
    setup(w, {"....", "...."});
    const Entity a = spawn_at(w, {0, 0});
    rebuild(w);
    const Entity late = spawn_at(w, {2, 1}); // spawned after the rebuild: not listed yet
    GridIndex &index = w.resources().get<GridIndex>();
    CHECK(index.at({2, 1}).empty());
    index.update(late, {2, 1}, {2, 1}); // erase is a no-op, then insert
    REQUIRE(index.at({2, 1}).size() == 1);
    CHECK(index.at({2, 1})[0] == late);
    index.update(late, {2, 1}, {3, 1});
    CHECK(index.at({2, 1}).empty());
    CHECK(index.at({3, 1}).size() == 1);
    index.update(a, {0, 0}, {-1, 0}); // off the grid: listed nowhere
    CHECK(index.at({0, 0}).empty());
}
