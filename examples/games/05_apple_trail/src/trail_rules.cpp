#include "trail_rules.h"

#include <algorithm>
#include <lam/core/id.h>
#include <lam/ecs/transform.h>
#include <lam/particles/components.h>
#include <lam/render/camera_math.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_mover.h>

namespace trail {
namespace {

constexpr lam::StrId kAppleStream = lam::str_id("trail.apples");

lam::TileWorld &tile_world(lam::World &w) {
    lam::install_grid_resources(w);                             // a bare World (unit tests) has none
    return w.resources().get<lam::TileWorld>();
}

// A new piece standing still on `cell`. GridMover.ticks is set per step by move_piece.
lam::Entity spawn_piece(lam::World &w, lam::IVec2 cell, std::uint16_t index) {
    const lam::TileWorld &tiles = tile_world(w);
    const lam::Entity e = w.create();
    w.add<lam::Transform>(e, lam::Transform{.position = tiles.tile_center(cell)});   // + PrevTransform
    w.add<lam::GridPosition>(e, lam::GridPosition{.cell = cell, .layer = 1, .blocks = true});
    w.add<lam::GridMover>(e, lam::GridMover{.from = cell, .to = cell, .ease = lam::Ease::Linear});
    w.add<Segment>(e, Segment{.index = index, .behind = {}});
    // Not added to GridIndex here: grid_index_rebuild (Simulate -1500) indexes it before the next
    // trail.step, and a piece born this step can never be hit on this step.
    return e;
}

void spawn_apple(lam::World &w, lam::IVec2 cell, AppleKind kind) {
    const lam::TileWorld &tiles = tile_world(w);
    const lam::Entity e = w.create();
    w.add<lam::Transform>(e, lam::Transform{.position = tiles.tile_center(cell)});
    // blocks = false: apples never stop try_step; the rules find them with a view on eat.
    w.add<lam::GridPosition>(e, lam::GridPosition{.cell = cell, .layer = 2, .blocks = false});
    w.add<Apple>(e, Apple{.kind = kind, .ticks_left = kind == AppleKind::Golden ? kRules.golden_ticks : 0});
}

// One cell along the grid. The rules have already checked the step, so set_grid_position(snap =
// false) moves the cell without checks, keeps GridIndex current and starts the GridMover tween,
// which the engine's grid_movers plays over `ticks` (Animate -1200).
void move_piece(lam::World &w, lam::Entity e, lam::IVec2 to, int ticks) {
    w.get<lam::GridMover>(e).ticks = static_cast<std::uint16_t>(ticks);
    lam::set_grid_position(w, e, to, /*snap=*/false);
}

// True if a trail piece other than `ignore` stands on `cell` (GridIndex: O(1) "who is here").
bool hits_trail(const lam::World &w, const lam::GridIndex &index, lam::IVec2 cell, lam::Entity ignore) {
    for (const lam::Entity e : index.at(cell))
        if (e != ignore && w.has<Segment>(e)) return true;
    return false;
}

bool golden_present(lam::World &w) {
    for (auto [e, apple] : w.view<Apple>())
        if (apple.kind == AppleKind::Golden) return true;
    return false;
}

void burst(const lam::Ctx &ctx, lam::AssetId<lam::EmitterConfig> TrailFx::*which, lam::IVec2 cell,
           std::uint16_t count) {
    const TrailFx *fx = ctx.try_res<TrailFx>();
    if (fx == nullptr) return;                                  // headless tests: no effects
    const lam::TileWorld &tiles = ctx.local<lam::TileWorld>();
    ctx.emit(lam::BurstAt{.config = fx->*which, .position = tiles.tile_center(cell), .count = count});
}

void rot_golden(const lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    lam::Entity rotted;
    for (auto [e, apple] : w.view<Apple>())
        if (apple.kind == AppleKind::Golden && --apple.ticks_left <= 0) rotted = e;
    if (rotted) w.destroy(rotted);                              // after the view: no structural change inside
}

void crash(const lam::Ctx &ctx, TrailState &s, lam::IVec2 at) {
    s.alive = false;
    ctx.emit(TrailCrashed{at});
    ctx.emit(lam::ShakeAt{.trauma = 0.9f});
    burst(ctx, &TrailFx::dust, at, 30);
}

// Moves every piece one cell: the head to `next`, each other piece into the cell ahead of it.
// A pending growth adds a piece where the tail was, so the tail seems to wait one step.
void advance(lam::World &w, TrailState &s, lam::IVec2 next) {
    lam::IVec2 vacated = next;
    for (lam::Entity e = s.head; e;) {
        const lam::IVec2 old = w.get<lam::GridPosition>(e).cell;
        move_piece(w, e, vacated, s.step_ticks);
        vacated = old;
        e = w.get<Segment>(e).behind;
    }
    if (s.grow > 0) {
        --s.grow;
        const lam::Entity piece = spawn_piece(w, vacated, static_cast<std::uint16_t>(s.length + 1));
        w.get<Segment>(s.tail).behind = piece;
        s.tail = piece;
        ++s.length;
    }
    ++s.steps;
}

void eat(const lam::Ctx &ctx, TrailState &s, lam::IVec2 cell) {
    lam::World &w = ctx.world();
    lam::Entity found;
    AppleKind kind = AppleKind::Red;
    for (auto [e, apple, at] : w.view<Apple, lam::GridPosition>()) {
        if (at.cell == cell) {
            found = e;
            kind = apple.kind;
        }
    }
    if (!found) return;
    w.destroy(found);

    const bool golden = kind == AppleKind::Golden;
    s.score += golden ? kRules.golden_points : kRules.red_points;
    s.grow += golden ? kRules.golden_growth : 1;
    ++s.eaten;
    s.step_ticks = step_ticks_for(s.eaten);
    ctx.emit(AppleEaten{cell, kind});
    ctx.emit(lam::ShakeAt{.trauma = golden ? 0.35f : 0.12f});
    burst(ctx, golden ? &TrailFx::golden_burst : &TrailFx::red_burst, cell, golden ? 28 : 14);
    if (golden) return;

    lam::Rng &rng = ctx.rng(kAppleStream);
    spawn_apple(w, random_free_cell(w, rng), AppleKind::Red);
    if (++s.reds_since_golden >= kRules.golden_every && !golden_present(w)) {
        s.reds_since_golden = 0;
        const lam::IVec2 at = random_free_cell(w, rng);
        spawn_apple(w, at, AppleKind::Golden);
        ctx.emit(GoldenAppeared{at});
    }
}

} // namespace

lam::IVec2 step(lam::IVec2 from, Dir dir) {
    switch (dir) {
    case Dir::Up: return {from.x, from.y - 1};
    case Dir::Down: return {from.x, from.y + 1};
    case Dir::Left: return {from.x - 1, from.y};
    case Dir::Right: return {from.x + 1, from.y};
    }
    return from;
}

bool opposite(Dir a, Dir b) {
    return (a == Dir::Up && b == Dir::Down) || (a == Dir::Down && b == Dir::Up) ||
           (a == Dir::Left && b == Dir::Right) || (a == Dir::Right && b == Dir::Left);
}

int step_ticks_for(int eaten) {
    return std::max(kRules.min_step_ticks, kRules.start_step_ticks - eaten / kRules.apples_per_speedup);
}

void queue_turn(TrailState &s, Dir dir) {
    if (!s.alive || s.queued_count >= s.queued.size()) return;
    const Dir last = s.queued_count > 0 ? s.queued[s.queued_count - 1u] : s.heading;
    if (dir == last || opposite(dir, last)) return;
    s.queued[s.queued_count++] = dir;
}

lam::IVec2 random_free_cell(lam::World &w, lam::Rng &rng) {
    const lam::TileWorld &tiles = tile_world(w);
    const int width = tiles.shapes.width();
    const int height = tiles.shapes.height();
    const auto is_free = [&](lam::IVec2 c) {
        if (tiles.at(c) != lam::TileShape::Empty) return false;
        for (auto [e, at] : w.view<lam::GridPosition>())        // pieces and apples
            if (at.cell == c) return false;
        return true;
    };
    for (int attempt = 0; attempt < 200; ++attempt) {
        const lam::IVec2 c{rng.between(0, width - 1), rng.between(0, height - 1)};
        if (is_free(c)) return c;
    }
    for (int y = 0; y < height; ++y)                            // a nearly full board
        for (int x = 0; x < width; ++x)
            if (is_free({x, y})) return {x, y};
    const TrailState *s = w.resources().try_get<TrailState>();
    return s != nullptr && w.alive(s->head) ? w.get<lam::GridPosition>(s->head).cell : lam::IVec2{};
}

void start_run(lam::World &w, const Orchard &orchard) {
    lam::TileWorld &tiles = tile_world(w);
    const std::uint32_t version = tiles.version;
    tiles = orchard.tiles;                                      // the board (copied: each run owns its world)
    tiles.version = version + 1;                                // a change, for anyone caching by version
    if (TrailState *old = w.resources().try_get<TrailState>()) {
        *old = TrailState{};                                    // restart on the same world
        w.clear();
    } else {
        w.resources().insert<TrailState>();
        w.snapshot_resource<TrailState>();                      // undo, saves and state_hash cover it
    }
    TrailState &s = w.resources().get<TrailState>();

    // The traveller faces right; the starting trail lies to its left (stacked where a wall is).
    const lam::IVec2 head = orchard.start;
    s.head = s.tail = spawn_piece(w, head, 0);
    lam::IVec2 last = head;
    for (int i = 1; i <= kRules.start_length; ++i) {
        const lam::IVec2 behind{head.x - i, head.y};
        if (!orchard.wall(behind)) last = behind;
        const lam::Entity piece = spawn_piece(w, last, static_cast<std::uint16_t>(i));
        w.get<Segment>(s.tail).behind = piece;
        s.tail = piece;
        ++s.length;
    }
    spawn_apple(w, random_free_cell(w, w.rng(kAppleStream)), AppleKind::Red);
}

void steer_system(lam::Ctx &ctx) {
    const Controls *c = ctx.try_res<Controls>();
    if (c == nullptr) return;
    const lam::InputFrame &in = lam::input_frame(ctx);         // latched: one tick per press
    TrailState &s = ctx.local<TrailState>();
    if (in.is_pressed(c->up)) queue_turn(s, Dir::Up);
    if (in.is_pressed(c->down)) queue_turn(s, Dir::Down);
    if (in.is_pressed(c->left)) queue_turn(s, Dir::Left);
    if (in.is_pressed(c->right)) queue_turn(s, Dir::Right);
}

void step_system(lam::Ctx &ctx) {
    TrailState &s = ctx.local<TrailState>();
    if (!s.alive) return;
    rot_golden(ctx);
    if (++s.tick_in_step < s.step_ticks) return;
    s.tick_in_step = 0;

    if (s.queued_count > 0) {
        s.heading = s.queued[0];
        s.queued[0] = s.queued[1];
        --s.queued_count;
    }
    lam::World &w = ctx.world();
    const lam::IVec2 at = w.get<lam::GridPosition>(s.head).cell;
    const lam::IVec2 next = step(at, s.heading);
    // The tail moves away on this same step, so its cell is free, unless the trail grows.
    const lam::Entity ignore = s.grow == 0 ? s.tail : lam::Entity{};
    if (ctx.local<lam::TileWorld>().at(next) == lam::TileShape::Solid ||
        hits_trail(w, ctx.local<lam::GridIndex>(), next, ignore)) {
        crash(ctx, s, at);
        return;
    }
    advance(w, s, next);
    eat(ctx, s, next);
}

void TrailRulesPlugin::build(lam::App &app) {
    app.require_plugin("lam.tilemap", name());                 // GridIndex, GridMover, grid_movers
    app.register_component<Segment>();
    app.register_component<Apple>();
    app.add_event<AppleEaten>();
    app.add_event<TrailCrashed>();
    app.add_event<GoldenAppeared>();
    app.add_event<lam::BurstAt>();                             // engine events emitted here: registering them is
    app.add_event<lam::ShakeAt>(lam::EventScope::World, 64);   // idempotent (ParticlesPlugin, RenderPlugin)
    app.add_system({.stage = lam::Stage::Input, .fn = steer_system, .name = "trail.steer",
                    .run_if = lam::has_local<TrailState>});
    app.add_system({.stage = lam::Stage::Simulate, .fn = step_system, .name = "trail.step",
                    .run_if = lam::has_local<TrailState>});
}

} // namespace trail
