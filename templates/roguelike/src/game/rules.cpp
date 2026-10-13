// Floors, turns, bump-to-attack, stairs and death, all driven by one Simulate system. Time is
// counted in turns, randomness comes from seeded Rngs, and effects leave as events (Hit, PlaySound).
#include <game/rules.h>

#include <cstdlib>
#include <vector>
#include <lam/audio/components.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/turns.h>
#include <lam/input/actions.h>
#include <lam/input/input_frame.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/fov.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/tilemap/procgen.h>

namespace game {
using namespace lam;
using namespace lam::literals;

namespace {
constexpr int kMapW = 40, kMapH = 24, kTile = 16; // TODO(level): map size, tile size
constexpr int kSight = 7;                          // FOV radius in cells

IVec2 center(const IRect &r) { return {r.x + r.w / 2, r.y + r.h / 2}; }

// A floor is a pure function of (seed, depth): BSP rooms joined by corridors. TileWorld isn't
// saved; the restore hook carves it again. TODO(level): cellular_caves, prefab rooms, doors.
std::vector<IRect> carve(TileWorld &tiles, std::uint64_t seed, std::int32_t depth) {
    Grid<std::uint8_t> grid(kMapW, kMapH, 0); // 0 = wall, 1 = floor
    std::vector<IRect> rooms;
    Rng rng(derive_seed(seed, static_cast<std::uint64_t>(depth)));
    bsp_rooms(grid, rng, BspParams{.min_room = 4, .max_room = 8, .depth = 4, .padding = 1}, rooms);
    if (rooms.empty()) { // shouldn't happen; one big room as a fallback
        rooms.push_back(IRect{2, 2, kMapW - 4, kMapH - 4});
        for (int y = 2; y < kMapH - 2; ++y)
            for (int x = 2; x < kMapW - 2; ++x) grid(x, y) = 1;
    }
    tiles.resize(kMapW, kMapH, kTile, Vec2{});
    for (int y = 0; y < kMapH; ++y)
        for (int x = 0; x < kMapW; ++x) {
            const bool floor = grid(x, y) != 0;
            tiles.set({x, y}, floor ? TileShape::Empty : TileShape::Solid, floor ? 0 : kTileOpaque);
            tiles.cost(x, y) = floor ? 10 : 0; // try_step and DijkstraMap treat cost 0 as a wall
        }
    return rooms;
}

void see(World &w) { // FOV from the hero; FovMemory also remembers every cell seen on this floor
    const RunState &run = w.resources().get<RunState>();
    update_fov(w.resources().get<FovMemory>(), w.resources().get<TileWorld>(),
               w.get<GridPosition>(run.player).cell, kSight);
}

// Destroys the old floor's entities, carves floor `run.depth`, places hero, stairs and monsters.
void build_floor(World &w) {
    RunState &run = w.resources().get<RunState>();
    std::vector<Entity> old; // destroy after the view, not inside it
    for (auto [e, pos] : w.view<GridPosition>(With<FloorLocal>{})) old.push_back(e);
    for (Entity e : old) w.destroy(e);
    TileWorld &tiles = w.resources().get<TileWorld>();
    const std::vector<IRect> rooms = carve(tiles, run.seed, run.depth);
    const IRect &last = rooms.back();
    run.stairs = rooms.size() > 1 ? center(last) : IVec2{last.x + last.w - 1, last.y + last.h - 1};
    // TODO(monsters): spawn tables, several kinds (prefab "extends"), items in the other rooms.
    for (std::size_t i = 1; i + 1 < rooms.size() && i <= static_cast<std::size_t>(run.depth) + 1; ++i) {
        const Entity m = w.prefabs()->spawn(w, "monster"_id, {});
        Stats &s = w.get<Stats>(m); // deeper floors: tougher monsters
        s.hp = s.max_hp = static_cast<std::int16_t>(s.max_hp + 2 * (run.depth - 1));
        set_grid_position(w, m, center(rooms[i])); // cell + Transform + GridIndex, no glide
    }
    set_grid_position(w, run.player, center(rooms.front()));
    if (GridIndex *index = w.resources().try_get<GridIndex>()) index->rebuild(w, tiles);
    w.resources().get<FovMemory>() = FovMemory{}; // forget the previous floor
    see(w);
}

void attack(const Ctx &ctx, Entity attacker, Entity target) {
    World &w = ctx.world();
    RunState &run = ctx.local<RunState>();
    // Named world stream: replays and loaded saves roll the same numbers.
    const int damage = w.get<Stats>(attacker).attack + ctx.rng("combat"_id).between(-1, 1);
    Stats &hurt = w.get<Stats>(target);
    hurt.hp = static_cast<std::int16_t>(hurt.hp - damage);
    const bool killed = hurt.hp <= 0;
    const IVec2 cell = w.get<GridPosition>(target).cell;
    ctx.emit(Hit{.cell = cell, .on_player = target == run.player, .killed = killed});
    ctx.emit(PlaySound{.group = "hit"_id, .position = ctx.local<TileWorld>().tile_center(cell)});
    if (target == run.player) {
        format_to(run.message, "The monster hits you for {}.", damage);
        if (killed) { // TODO(rules): a game-over menu scene (RECIPES 2.9), save erase, high score
            run.phase = Phase::Dead;
            format_to(run.message, "You died on depth {}. Space: new run", run.depth);
        }
        return;
    }
    format_to(run.message, "You hit the monster for {}.", damage);
    if (!killed) return;
    ++run.kills;
    run.score += 10 * run.depth; // TODO(rules): xp, levels, loot drops
    run.message = "The monster dies.";
    w.destroy(target); // no view open here
}

// The hero's action this tick, from the latched InputFrame. Returns false while there is nothing
// to do (no key, or a wall): the turn is not spent and the scheduler asks again next tick.
bool hero_acts(const Ctx &ctx, Entity hero) {
    World &w = ctx.world();
    RunState &run = ctx.local<RunState>();
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx); // is_repeated: the press plus key repeat while held
    if (in.is_repeated(c.wait)) return true; // rest one turn (TODO(rules): heal while resting)
    const IVec2 dir = in.is_repeated(c.up)     ? IVec2{0, -1}
                      : in.is_repeated(c.down) ? IVec2{0, 1}
                      : in.is_repeated(c.left) ? IVec2{-1, 0}
                      : in.is_repeated(c.right) ? IVec2{1, 0}
                                                : IVec2{};
    if (dir == IVec2{}) return false;
    Entity blocker;
    switch (try_step(w, hero, dir, {}, &blocker)) {
    case StepResult::Moved: // GridMover animates the step; TurnScheduler stays busy meanwhile
        if (w.get<GridPosition>(hero).cell != run.stairs) {
            see(w);
            return true;
        }
        ++run.depth; // TODO(rules): require a key for the stairs, a win at depth N
        run.score += 100;
        format_to(run.message, "You descend to depth {}.", run.depth);
        build_floor(w);
        return true;
    case StepResult::BlockedEntity: // bump-to-attack (GridIndex found who stands there)
        if (!w.alive(blocker) || !w.has<Stats>(blocker)) return false;
        attack(ctx, hero, blocker);
        return true;
    default: // a wall, or our own step still animating
        return false;
    }
}

// A monster's turn: asleep until the hero can see it (FOV is symmetric), then chase and bite.
// TODO(monsters): wander when asleep (ctx.rng("ai"_id)), a shared DijkstraMap chase (see the
// reference game), ranged attacks, an Fsm<State> per kind.
void monster_acts(const Ctx &ctx, Entity m) {
    World &w = ctx.world();
    const RunState &run = ctx.local<RunState>();
    const IVec2 me = w.get<GridPosition>(m).cell;
    const IVec2 to_hero = w.get<GridPosition>(run.player).cell - me;
    if (!cell_visible(w, me)) return;
    if (std::abs(to_hero.x) + std::abs(to_hero.y) == 1) return attack(ctx, m, run.player);
    const IVec2 step_x{to_hero.x > 0 ? 1 : -1, 0}, step_y{0, to_hero.y > 0 ? 1 : -1};
    const bool x_first = std::abs(to_hero.x) >= std::abs(to_hero.y); // greedy: longer axis first
    if (try_step(w, m, x_first ? step_x : step_y) != StepResult::Moved && (x_first ? to_hero.y : to_hero.x) != 0)
        (void)try_step(w, m, x_first ? step_y : step_x);
}

// Stage::Simulate. next_actor() hands out turns by energy (TurnActor::speed); it returns null while
// a GridMover animates (busy), so the world waits for steps to finish before the next turn.
void take_turns(Ctx &ctx) {
    World &w = ctx.world();
    RunState &run = ctx.local<RunState>();
    if (run.phase == Phase::Dead) {
        if (input_frame(ctx).is_pressed(ctx.res<Controls>().wait)) start_run(w, derive_seed(run.seed, 1));
        return;
    }
    const std::int32_t depth = run.depth;
    bool hero_done = false;
    for (Entity e; (e = next_actor(w));) {
        if (w.has<PlayerTurn>(e)) {
            if (hero_done || !hero_acts(ctx, e)) break; // one hero action per tick, or wait for input
            hero_done = true;
        } else {
            monster_acts(ctx, e);
        }
        end_turn(w, e);
        if (run.phase != Phase::Playing || run.depth != depth) break; // died, or a new floor
    }
}

// Restore hook (History::undo, deserialize_world): TileWorld is derived from RunState, rebuild it.
void rebuild_after_restore(World &w) {
    const RunState *run = w.resources().try_get<RunState>();
    TileWorld *tiles = w.resources().try_get<TileWorld>();
    if (run == nullptr || tiles == nullptr) return;
    (void)carve(*tiles, run->seed, run->depth);
    if (GridIndex *index = w.resources().try_get<GridIndex>()) index->rebuild(w, *tiles);
}
} // namespace

void RulesPlugin::build(App &app) {
    app.require_plugin("lam.input", name());
    app.require_plugin("lam.tilemap", name());
    // Prefab JSON can only name registered components.
    app.register_component<Player>().register_component<FloorLocal>().register_component<Stats>();
    app.add_event<Hit>();
    app.add_event<PlaySound>(); // idempotent; lets the rules run without AudioPlugin (tests)
    ActionMap &map = app.res<ActionMap>(); // TODO(input): more actions (pick up, use, stairs)
    const auto k = [](Key key) { return ActionMap::key(key); };
    const Controls c{.up = map.button("up", {k(Key::Up), k(Key::W), k(Key::Kp8)}),
                     .down = map.button("down", {k(Key::Down), k(Key::S), k(Key::Kp2)}),
                     .left = map.button("left", {k(Key::Left), k(Key::A), k(Key::Kp4)}),
                     .right = map.button("right", {k(Key::Right), k(Key::D), k(Key::Kp6)}),
                     .wait = map.button("wait", {k(Key::Space), k(Key::Kp5)})};
    for (ButtonAction a : {c.up, c.down, c.left, c.right, c.wait}) map.set_repeat(a, 12, 6); // held keys
    app.insert_resource<Controls>(c);
    app.add_system({.stage = Stage::Simulate, .fn = take_turns, .name = "game.turns",
                    .run_if = &has_local<RunState>}); // menu or overlay worlds are skipped
}

void RulesPlugin::init_world(App &app, World &world) {
    (void)app;
    world.add_restore_hook(&rebuild_after_restore);
}

void start_run(World &w, std::uint64_t seed) {
    Resources &res = w.resources();
    if (!res.has<RunState>()) { // first run on this world: register the saved + hashed state once
        res.insert<RunState>();
        w.snapshot_resource<RunState>();
        res.insert<FovMemory>();
        w.snapshot_resource<FovMemory>();
        add_turn_scheduler(w);
    }
    RunState &run = res.get<RunState>();
    if (w.alive(run.player)) w.destroy(run.player);
    run = RunState{.seed = seed};
    res.get<TurnScheduler>() = TurnScheduler{};
    run.player = w.prefabs()->spawn(w, "player"_id, {});
    run.message = "Find the stairs. Bump into monsters to fight.";
    build_floor(w);
}

bool cell_visible(const World &w, IVec2 cell) {
    const FovMemory *fov = w.resources().try_get<FovMemory>();
    return fov != nullptr && fov->visible.get_or(cell, 0) != 0;
}

bool cell_explored(const World &w, IVec2 cell) {
    const FovMemory *fov = w.resources().try_get<FovMemory>();
    return fov != nullptr && fov->explored.get_or(cell, 0) != 0;
}
} // namespace game
