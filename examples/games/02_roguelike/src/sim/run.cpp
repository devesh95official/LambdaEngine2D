// RulesPlugin and run setup: which World resources a roguelike run needs, which of them undo and
// saves must cover (World::snapshot_resource), and the two Simulate systems.
#include <sim/run.h>

#include <lam/assets/asset_server.h>
#include <lam/audio/components.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/turns.h>
#include <lam/input/actions.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/fov.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/tilemap/tilemap.h>
#include <lam/ui/message_log.h>
#include <sim/controls.h>
#include <sim/rules.h>

namespace rl {

void RulesPlugin::build(lam::App &app) {
    app.require_plugin("lam.tilemap", name()); // GridIndex, TilemapInstance, set_tile
    app.require_plugin("lam.input", name());   // ActionMap, InputFrame
    app.register_component<Player>();
    app.register_component<FloorLocal>();
    app.register_component<Monster>();
    app.register_component<Stats>();
    app.register_component<Item>();
    app.register_component<Inventory>();
    app.add_event<Hit>(); // World scope: per world, read by presentation
    // Engine event the rules emit; idempotent, so the rules also run without AudioPlugin.
    app.add_event<lam::PlaySound>();
    if (lam::ActionMap *map = app.try_res<lam::ActionMap>(); map && !app.try_res<Controls>())
        app.insert_resource<Controls>(define_controls(*map));
    // Only worlds holding a run; menu worlds and the idle main world are skipped.
    app.add_system({.stage = lam::Stage::Simulate,
                    .fn = generate_floor_system,
                    .name = "rl.generate_floor",
                    .order = 0,
                    .run_if = lam::has_local<RunState>});
    app.add_system({.stage = lam::Stage::Simulate,
                    .fn = take_turns,
                    .name = "rl.take_turns",
                    .order = 10,
                    .run_if = lam::has_local<RunState>});
}

namespace {
void invalidate_chase(lam::World &w) { // restore hook: the cache is derived state
    if (ChaseMap *c = w.resources().try_get<ChaseMap>()) c->valid = false;
}
template <class T>
T &ensure(lam::World &w) {
    if (T *p = w.resources().try_get<T>()) return *p;
    return w.resources().insert<T>();
}
} // namespace

void setup_run(lam::App &app, lam::World &w, std::uint64_t run_seed) {
    RunState &run = ensure<RunState>(w);
    run = RunState{};
    run.run_seed = run_seed;
    // Saved and hashed state that isn't a component: register once per world.
    w.snapshot_resource<RunState>();
    lam::add_turn_scheduler(w); // TurnScheduler + snapshot_resource
    ensure<lam::FovMemory>(w);
    w.snapshot_resource<lam::FovMemory>();
    if (!w.resources().has<lam::MessageLog>()) w.resources().insert<lam::MessageLog>(64);
    ensure<ChaseMap>(w);
    w.add_restore_hook(&invalidate_chase);

    // The dungeon is a Tilemap entity over an all-wall template; tilemap_sync (FrameBegin -1000)
    // turns it into a TilemapInstance + TileWorld, then generate_floor carves it with set_tile.
    const lam::Entity map = w.create();
    w.add<lam::Transform>(map);
    w.add<lam::Tilemap>(
        map,
        lam::Tilemap{.map = app.res<lam::AssetServer>().load<lam::TilemapAsset>("maps/floor.tmj"),
                     .build_collision = true,
                     .spawn_objects = false});
    run.map = map;
    run.player = app.res<lam::PrefabLibrary>().spawn(w, "player"_id, {});
    say(w, "Welcome! Find the stairs (>) and dive as deep as you dare.",
        lam::Color{250, 220, 120, 255});
}

const RunState *run_state(const lam::World &w) {
    return w.resources().try_get<RunState>();
}

bool cell_visible(const lam::World &w, lam::IVec2 cell) {
    const lam::FovMemory *fov = w.resources().try_get<lam::FovMemory>();
    return fov && fov->visible.get_or(cell, 0) != 0;
}

bool cell_explored(const lam::World &w, lam::IVec2 cell) {
    const lam::FovMemory *fov = w.resources().try_get<lam::FovMemory>();
    return fov && fov->explored.get_or(cell, 0) != 0;
}

std::uint64_t score_of(const lam::World &w) {
    const RunState *run = run_state(w);
    return run ? run->score : 0;
}

void say(lam::World &w, std::string_view text, lam::Color c) {
    if (lam::MessageLog *log = w.resources().try_get<lam::MessageLog>()) log->add(text, c);
}

void refresh_fov(lam::World &w) {
    const RunState *run = run_state(w);
    lam::FovMemory *fov = w.resources().try_get<lam::FovMemory>();
    const lam::TileWorld *tiles = w.resources().try_get<lam::TileWorld>();
    if (!run || !fov || !tiles || !w.alive(run->player)) return;
    const lam::IVec2 eye = w.get<lam::GridPosition>(run->player).cell;
    lam::update_fov(*fov, *tiles, eye, kFovRadius); // sizes both grids to the TileWorld
    // Shadowcasting is symmetric: a monster standing in a visible cell can see the player too.
    for (auto [e, m, pos] : w.view<Monster, lam::GridPosition>()) {
        if (m.awake || !cell_visible(w, pos.cell) || lam::chebyshev(pos.cell, eye) > m.sight)
            continue;
        m.awake = true;
        lam::FixedString<96> line;
        lam::format_to(line, "The {} notices you.", m.name.view());
        say(w, line.view(), lam::Color{230, 180, 120, 255});
    }
}

const ChaseMap &chase_map(lam::World &w, lam::IVec2 player_cell) {
    ChaseMap &c = w.resources().get<ChaseMap>();
    const lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    if (c.valid && c.goal == player_cell && c.tile_version == tiles.version) return c;
    const int width = tiles.shapes.width(), height = tiles.shapes.height();
    if (c.cost.width() != width || c.cost.height() != height) {
        c.cost.resize(width, height, 0); // allocates only when the map size changes
        c.map.resize({width, height});
    }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            c.cost(x, y) =
                tiles.shapes(x, y) == lam::TileShape::Solid ? std::uint16_t{0} : std::uint16_t{10};
    const lam::IVec2 goals[1] = {player_cell};
    c.map.compute(c.cost, goals, lam::GridDirs::Four);
    c.goal = player_cell;
    c.tile_version = tiles.version;
    c.valid = true;
    return c;
}
} // namespace rl
