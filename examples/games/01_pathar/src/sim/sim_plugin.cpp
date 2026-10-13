// Pathar's simulation systems (see sim_plugin.h).
#include "sim/sim_plugin.h"

#include <lam/audio/components.h>
#include <lam/ecs/world.h>
#include <lam/input/keys.h>
#include <lam/particles/components.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_mover.h>
#include "sim/board.h"

namespace pathar {

namespace {

using lam::ActionMap;
using lam::Key;
using lam::PadButton;

using Kind = PuzzleEvent::Kind;

void sound(const lam::Ctx &ctx, lam::StrId group, float volume = 1.0f) {
    if (ctx.local<Board>().quiet) return;
    ctx.emit(lam::PlaySound{.group = group, .volume = volume});
}
void sound(const lam::Ctx &ctx, std::string_view group, float volume = 1.0f) {
    sound(ctx, lam::str_id(group), volume);
}

// BurstAt names a particle config by AssetId; without PatharFx (bare tests) there are no bursts.
void burst(const lam::Ctx &ctx, lam::AssetId<lam::EmitterConfig> PatharFx::*which, lam::Vec2 at,
           std::uint16_t count) {
    const PatharFx *fx = ctx.try_res<PatharFx>();
    if (fx == nullptr || !(fx->*which)) return;
    ctx.emit(lam::BurstAt{.config = fx->*which, .position = at, .count = count});
}

void event(const lam::Ctx &ctx, Kind kind, lam::IVec2 cell) { ctx.emit(PuzzleEvent{.kind = kind, .cell = cell}); }

lam::IVec2 hero_cell(lam::World &w) {
    const lam::Entity hero = player(w);
    return hero ? w.get<lam::GridPosition>(hero).cell : lam::IVec2{};
}

void clear_route(PuzzleState &ps) {
    ps.queued = 0;
    ps.path_len = ps.path_pos = 0;
}

void do_undo(const lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    if (undo(w)) {
        sound(ctx, "undo", 0.6f);
        event(ctx, Kind::Undo, hero_cell(w));
    } else {
        sound(ctx, "ui_error", 0.5f);
        event(ctx, Kind::NothingToUndo, hero_cell(w));
    }
}

void do_restart(const lam::Ctx &ctx, bool keep_history) {
    restart(ctx.world(), keep_history);
    sound(ctx, "gate_open", 0.5f);
    if (keep_history) event(ctx, Kind::Restart, hero_cell(ctx.world()));  // "Z brings it back"
}

// A click: a stone right beside the player is pushed; anywhere else reachable is walked to.
void click(lam::World &w, PuzzleState &ps, lam::Vec2 world_point) {
    const Board &b = w.resources().get<Board>();
    const lam::IVec2 cell = w.resources().get<lam::TileWorld>().tile_at(world_point);
    if (!b.level.inside(cell)) return;
    const lam::IVec2 me = hero_cell(w);
    for (Dir d : kDirs) {
        if (me + dir_vec(d) == cell && stone_at(w, cell)) {
            clear_route(ps);
            ps.queued = static_cast<std::uint8_t>(1 + static_cast<int>(d));
            return;
        }
    }
    const std::vector<Dir> route = walk_path(b.level, read_state(w), b.level.cell(cell));
    if (route.empty() || route.size() > static_cast<std::size_t>(kMaxPath)) return;
    clear_route(ps);
    for (std::size_t i = 0; i < route.size(); ++i) ps.path[i] = static_cast<std::uint8_t>(route[i]);
    ps.path_len = static_cast<std::uint8_t>(route.size());
}

}  // namespace

lam::ButtonAction Controls::dir(Dir d) const {
    switch (d) {
    case Dir::Up: return up;
    case Dir::Down: return down;
    case Dir::Left: return left;
    case Dir::Right: return right;
    }
    return {};
}

Controls define_controls(ActionMap &map) {
    Controls c;
    c.up = map.button("pathar.up", {ActionMap::key(Key::W), ActionMap::key(Key::Up), ActionMap::pad(PadButton::DUp)});
    c.down = map.button("pathar.down", {ActionMap::key(Key::S), ActionMap::key(Key::Down), ActionMap::pad(PadButton::DDown)});
    c.left = map.button("pathar.left", {ActionMap::key(Key::A), ActionMap::key(Key::Left), ActionMap::pad(PadButton::DLeft)});
    c.right = map.button("pathar.right",
                         {ActionMap::key(Key::D), ActionMap::key(Key::Right), ActionMap::pad(PadButton::DRight)});
    c.undo = map.button("pathar.undo", {ActionMap::key(Key::Z), ActionMap::key(Key::U), ActionMap::key(Key::Backspace),
                                        ActionMap::pad(PadButton::West)});
    c.restart = map.button("pathar.restart", {ActionMap::key(Key::R), ActionMap::pad(PadButton::Select)});
    c.hint = map.button("pathar.hint", {ActionMap::key(Key::H), ActionMap::pad(PadButton::North)});
    c.pause = map.button("pathar.pause", {ActionMap::key(Key::Escape), ActionMap::key(Key::P), ActionMap::pad(PadButton::Start)});
    c.point = map.button("pathar.point", {ActionMap::mouse(lam::MouseButton::Left)});
    // Held keys repeat in ticks: walking every glide, undo quickly, hints slowly.
    for (Dir d : kDirs) map.set_repeat(c.dir(d), 12, kGlideTicks);
    map.set_repeat(c.undo, 18, 4);
    map.set_repeat(c.hint, 21, 21);
    return c;
}

void puzzle_input(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    PuzzleState &ps = ctx.local<PuzzleState>();  // resources never move: valid across restore
    const Controls &c = ctx.res<Controls>();
    const lam::InputFrame &in = lam::input_frame(ctx);
    // Intents first: they are UI decisions taken last frame (pause menu, solved menu).
    for (const lam::Intent &i : in.intent_list()) {
        if (i.kind == kIntentReplay) do_restart(ctx, false);
        else if (i.kind == kIntentRestart && !ps.solved && ps.moves > 0) do_restart(ctx, true);
        else if (i.kind == kIntentUndo && !ps.solved) do_undo(ctx);
    }
    if (ps.solved) return;
    if (in.is_repeated(c.undo)) {
        do_undo(ctx);
        return;
    }
    if (in.is_pressed(c.restart) && ps.moves > 0) {
        do_restart(ctx, true);
        return;
    }
    for (Dir d : kDirs) {
        if (in.is_repeated(c.dir(d))) {  // pressed, or held past the repeat delay
            clear_route(ps);
            ps.queued = static_cast<std::uint8_t>(1 + static_cast<int>(d));
        }
    }
    // A hint: the first step of a solution from here. Solving is deterministic, so it may run in
    // a tick system (and replays reproduce it).
    if (in.is_repeated(c.hint) && ps.queued == 0 && ps.path_pos >= ps.path_len && !busy(w)) {
        const Board &b = ctx.local<Board>();
        const Solution sol = solve(b.level, read_state(w));
        if (sol.found && !sol.moves.empty()) {
            ps.queued = static_cast<std::uint8_t>(1 + static_cast<int>(sol.moves.front()));
            ps.helped = true;
        } else {
            sound(ctx, "ui_error");
            event(ctx, Kind::NoHint, hero_cell(w));
        }
    }
    if (in.is_pressed(c.point) && in.pointer_valid) click(w, ps, in.pointer_world);
}

void puzzle_move(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    PuzzleState &ps = ctx.local<PuzzleState>();
    if (ps.solved || busy(w)) return;  // one step at a time: wait for the glide
    Dir d = Dir::Up;
    if (ps.queued != 0) {
        d = static_cast<Dir>(ps.queued - 1);
        ps.queued = 0;
    } else if (ps.path_pos < ps.path_len) {
        d = static_cast<Dir>(ps.path[ps.path_pos++]);
    } else {
        return;
    }
    const Board &b = ctx.local<Board>();
    const MoveResult r = step(w, d);
    if (!r.moved) {
        sound(ctx, "bump", 0.5f);
        event(ctx, Kind::Bump, hero_cell(w));
        clear_route(ps);
        return;
    }
    const Level &l = b.level;
    sound(ctx, b.footstep, 0.45f);
    event(ctx, Kind::Moved, hero_cell(w));
    if (r.pushed) {
        sound(ctx, "stone_push", 0.8f);
        burst(ctx, &PatharFx::dust, cell_center(w, l.pos(r.from)) + lam::Vec2{0, 6}, 6);
        event(ctx, Kind::Pushed, l.pos(r.to));
        if (r.sank) {
            sound(ctx, "stone_splash");
            burst(ctx, &PatharFx::splash, cell_center(w, l.pos(r.to)), 16);
            event(ctx, Kind::Sank, l.pos(r.to));
        }
        if (r.onto_goal) {
            sound(ctx, "stone_set", 0.9f);
            event(ctx, Kind::OntoGoal, l.pos(r.to));
        } else if (r.off_goal) {
            sound(ctx, "lamp_wrong", 0.5f);
            event(ctx, Kind::OffGoal, l.pos(r.from));
        }
    }
    const State s = read_state(w);
    if (solved(l, s)) {
        ps.solved = true;
        ps.solved_tick = static_cast<std::uint32_t>(w.tick());
        clear_route(ps);
        sound(ctx, "puzzle_solved");
        for (int c = 0; c < l.width * l.height; ++c) {
            if (l.is_goal(c)) burst(ctx, &PatharFx::sparkle, cell_center(w, l.pos(c)), 18);
        }
        event(ctx, Kind::Solved, hero_cell(w));
    } else if (stuck(l, s, b.dead)) {
        if (r.pushed && !ps.stuck_said) {
            ps.stuck_said = true;
            event(ctx, Kind::Stuck, hero_cell(w));
        }
    } else {
        ps.stuck_said = false;
    }
}

void PatharSimPlugin::build(lam::App &app) {
    app.require_plugin("lam.tilemap", name());  // GridIndex, grid_movers (glides)
    app.require_plugin("lam.physics", name());  // TileWorld
    app.require_plugin("lam.input", name());    // ActionMap, ActionState
    app.register_component<Player>();
    app.register_component<Stone>();
    app.register_component<Sunk>();
    app.register_component<Facing>();
    app.add_event<PuzzleEvent>();
    app.add_event<lam::PlaySound>();  // idempotent: audio and particles register them too
    app.add_event<lam::BurstAt>();
    if (app.try_res<Controls>() == nullptr) app.insert_resource<Controls>(define_controls(app.res<lam::ActionMap>()));
    // run_if keeps the systems off worlds without a puzzle (menus, the title's overlay).
    app.add_system({.stage = lam::Stage::Input, .fn = puzzle_input, .name = "pathar.input",
                    .run_if = lam::has_local<Board>});
    app.add_system({.stage = lam::Stage::Simulate, .fn = puzzle_move, .name = "pathar.move", .order = 0,
                    .run_if = lam::has_local<Board>});
}

}  // namespace pathar
