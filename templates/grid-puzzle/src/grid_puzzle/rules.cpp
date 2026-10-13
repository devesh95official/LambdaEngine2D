// grid_puzzle rules: the pure kernel, the level loader and the one tick system (see rules.h).
#include "grid_puzzle/rules.h"

#include <algorithm>
#include <format>
#include <lam/audio/components.h>
#include <lam/core/log.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/particles/components.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_mover.h>

namespace grid_puzzle {

using namespace lam::literals;
using lam::Entity;
using lam::IVec2;
using lam::World;

// ---- 1. Pure rules ----------------------------------------------------------------------------

lam::Result<std::vector<Level>> parse_levels(std::string_view text) {
    std::vector<Level> levels;
    std::vector<std::string_view> rows;                         // the level being read
    const auto finish = [&]() -> lam::Status {
        if (rows.empty()) return {};
        Level l;
        std::size_t width = 0;
        for (std::string_view r : rows) width = std::max(width, r.size());
        l.cells.resize(static_cast<int>(width), static_cast<int>(rows.size()), ' ');
        int players = 0, goals = 0;
        for (int y = 0; y < static_cast<int>(rows.size()); ++y) {
            for (int x = 0; x < static_cast<int>(rows[y].size()); ++x) {
                const char c = rows[y][x];
                if (std::string_view("# .@$*+").find(c) == std::string_view::npos)
                    return lam::make_error(lam::ErrorCode::Parse,
                                           std::format("level {}: unknown character '{}'", levels.size() + 1, c));
                const bool on_goal = c == '.' || c == '*' || c == '+';
                l.cells(x, y) = c == '#' ? '#' : on_goal ? '.' : ' ';   // the board keeps only static cells
                goals += on_goal ? 1 : 0;
                if (c == '@' || c == '+') {
                    ++players;
                    l.start.player = {x, y};
                }
                if (c == '$' || c == '*') l.start.crates.push_back({x, y});
            }
        }
        if (players != 1 || l.start.crates.empty() || goals < static_cast<int>(l.start.crates.size()))
            return lam::make_error(lam::ErrorCode::Parse,
                                   std::format("level {}: needs one player, a crate, and a goal per crate",
                                               levels.size() + 1));
        levels.push_back(std::move(l));
        rows.clear();
        return {};
    };
    for (std::size_t start = 0; start <= text.size();) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        start = end + 1;
        if (line.starts_with(';')) continue;                    // comment
        if (line.find_first_not_of(' ') == std::string_view::npos) LAM_TRY(finish());   // blank: level ends
        else rows.push_back(line);
    }
    LAM_TRY(finish());
    if (levels.empty()) return lam::make_error(lam::ErrorCode::Parse, "no levels found");
    return levels;
}

Move try_move(const Level &level, State &s, IVec2 dir) {
    const IVec2 to = s.player + dir;
    if (level.wall(to)) return Move::Blocked;
    const auto crate = std::find(s.crates.begin(), s.crates.end(), to);
    if (crate == s.crates.end()) {
        s.player = to;
        return Move::Walk;
    }
    const IVec2 beyond = to + dir;                              // a crate moves only into free floor
    if (level.wall(beyond) || std::find(s.crates.begin(), s.crates.end(), beyond) != s.crates.end())
        return Move::Blocked;
    *crate = beyond;
    s.player = to;
    return Move::Push;
}

bool solved(const Level &level, const State &s) {
    return std::all_of(s.crates.begin(), s.crates.end(), [&](IVec2 c) { return level.goal(c); });
}

// ---- 2. The ECS game ----------------------------------------------------------------------------

namespace {

void spawn_pieces(World &w, const Level &level) {
    std::vector<Entity> old;                                    // collect first: no structural change in a view
    for (auto [e, gp] : w.view<lam::GridPosition>()) old.push_back(e);
    for (Entity e : old) w.destroy(e);
    const lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    const auto spawn = [&](IVec2 cell) {
        const Entity e = w.create();
        // Transform starts on the cell centre; grid_movers (Animate -1200) glides it on each step.
        w.add<lam::Transform>(e, lam::Transform{.position = tiles.tile_center(cell)});
        w.add<lam::GridPosition>(e, lam::GridPosition{.cell = cell});
        w.add<lam::GridMover>(e, lam::GridMover{.from = cell, .to = cell, .ticks = kStepTicks});
        return e;
    };
    w.add<Player>(spawn(level.start.player));
    for (IVec2 c : level.start.crates) w.add<Crate>(spawn(c));
}

// Simulate, on worlds with a Run. Input is read here too: the InputFrame is latched, so each
// press lands on exactly one tick.
void play(lam::Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();                                // resources never move: valid across undo
    lam::History &history = ctx.local<Undo>().history;
    const Controls &c = ctx.res<Controls>();
    const std::vector<Level> &levels = ctx.res<Levels>().list;
    const Level &level = levels[static_cast<std::size_t>(run.level)];
    const lam::InputFrame &in = lam::input_frame(ctx);          // neutral when this world is not focused

    if (in.is_repeated(c.undo)) {                               // held: undo again every few ticks
        history.undo(w);                                        // pieces, Run and all; nothing to undo = no-op
        return;
    }
    if (run.solved) {
        if (in.is_pressed(c.next)) {
            const auto next = static_cast<std::int32_t>((static_cast<std::size_t>(run.level) + 1) % levels.size());
            load_level(w, levels[static_cast<std::size_t>(next)], next);
        }
        return;
    }
    if (in.is_pressed(c.restart) && run.moves > 0) {
        history.push(w);                                        // so Z brings the old position back
        spawn_pieces(w, level);
        run = Run{.level = run.level, .cleared = run.cleared};
        return;
    }
    for (std::size_t i = 0; i < kDirs.size(); ++i)              // remember a tap made during a glide
        if (in.is_repeated(c.dirs()[i])) run.queued = static_cast<std::uint8_t>(i + 1);
    if (run.queued == 0 || busy(w)) return;                     // one step at a time
    const IVec2 dir = kDirs[run.queued - 1u];
    run.queued = 0;

    // Decide on a copy so undo only ever stores legal moves.
    State after = read_state(w);
    const IVec2 from = after.player;
    const Move move = try_move(level, after, dir);
    if (move == Move::Blocked) return;                          // TODO(feel): a bump sound or a shake event
    history.push(w);
    Entity player, crate;
    for (auto [e, gp] : w.view<lam::GridPosition>()) {
        if (w.has<Player>(e)) player = e;
        else if (gp.cell == from + dir) crate = e;
    }
    // Crate first so its cell is free for the player. try_step updates GridIndex and starts the glide.
    if (move == Move::Push && lam::try_step(w, crate, dir) != lam::StepResult::Moved)
        LAM_LOG_WARN("grid_puzzle: try_step refused a push the rules allowed");
    if (lam::try_step(w, player, dir) != lam::StepResult::Moved)
        LAM_LOG_WARN("grid_puzzle: try_step refused a step the rules allowed");
    ++run.moves;
    if (move == Move::Push) {
        ++run.pushes;
        // Effects go out as events; audio and particles pick them up in Update.
        const lam::Vec2 at = ctx.local<lam::TileWorld>().tile_center(from + dir + dir);
        ctx.emit(lam::PlaySound{.group = "push"_id, .position = at});
        if (const Fx *fx = ctx.try_res<Fx>()) ctx.emit(lam::BurstAt{.config = fx->dust, .position = at, .count = 10});
    }
    if (solved(level, after)) {
        run.solved = true;
        ++run.cleared;
    }
}

}  // namespace

void load_level(World &w, const Level &level, std::int32_t index) {
    lam::Resources &r = w.resources();
    if (!r.has<Run>()) {
        r.insert<Run>();
        w.snapshot_resource<Run>();                             // undo, saves and state_hash cover Run
    }
    if (!r.has<Undo>()) r.insert<Undo>();
    if (!r.has<lam::TileWorld>()) r.insert<lam::TileWorld>();   // PhysicsPlugin inserts it; bare worlds do not
    // Walls are Solid tiles, which try_step refuses. set() writes only shape and flags, so walls
    // also get cost 0, or a PathFinder (a hint or an enemy, later) would walk through them.
    // TODO(save/load): the TileWorld is derived from Run::level; rebuild it in a restore hook.
    lam::TileWorld &tiles = r.get<lam::TileWorld>();
    tiles.resize(level.cells.width(), level.cells.height(), kTile, {});   // every cost back to 10
    for (int y = 0; y < level.cells.height(); ++y) {
        for (int x = 0; x < level.cells.width(); ++x) {
            if (!level.wall({x, y})) continue;
            tiles.set({x, y}, lam::TileShape::Solid, 0);
            tiles.cost(x, y) = 0;
        }
    }
    spawn_pieces(w, level);
    Run &run = r.get<Run>();
    run = Run{.level = index, .cleared = run.cleared};
    r.get<Undo>().history.clear();                              // no undoing into the previous level
}

State read_state(World &w) {
    State s;
    for (auto [e, gp] : w.view<lam::GridPosition>()) {
        if (w.has<Player>(e)) s.player = gp.cell;
        else if (w.has<Crate>(e)) s.crates.push_back(gp.cell);
    }
    return s;
}

bool busy(World &w) {
    for (auto [e, mover] : w.view<lam::GridMover>())
        if (mover.moving) return true;
    return false;
}

void RulesPlugin::build(lam::App &app) {
    app.require_plugin("lam.tilemap", name());                  // GridIndex and grid_movers (the glide)
    app.require_plugin("lam.input", name());                    // ActionMap, InputFrame
    app.register_component<Player>().register_component<Crate>();
    app.add_event<lam::PlaySound>();                            // idempotent: lets the rules run in apps
    app.add_event<lam::BurstAt>();                              // without the audio/particles plugins
    lam::ActionMap &map = app.res<lam::ActionMap>();
    const auto key = [](lam::Key k) { return lam::ActionMap::key(k); };
    const auto pad = [](lam::PadButton b) { return lam::ActionMap::pad(b); };
    // TODO(controls): default keys. Players can rebind; InputPlugin saves bindings in the settings file.
    Controls c{.up = map.button("up", {key(lam::Key::W), key(lam::Key::Up), pad(lam::PadButton::DUp)}),
               .right = map.button("right", {key(lam::Key::D), key(lam::Key::Right), pad(lam::PadButton::DRight)}),
               .down = map.button("down", {key(lam::Key::S), key(lam::Key::Down), pad(lam::PadButton::DDown)}),
               .left = map.button("left", {key(lam::Key::A), key(lam::Key::Left), pad(lam::PadButton::DLeft)}),
               .undo = map.button("undo", {key(lam::Key::Z), key(lam::Key::Backspace), pad(lam::PadButton::West)}),
               .restart = map.button("restart", {key(lam::Key::R), pad(lam::PadButton::Select)}),
               .next = map.button("next", {key(lam::Key::Enter), key(lam::Key::Space), pad(lam::PadButton::South)})};
    for (lam::ButtonAction d : c.dirs()) map.set_repeat(d, 12, kStepTicks);   // hold to keep walking
    map.set_repeat(c.undo, 18, 4);                              // hold to rewind quickly
    app.insert_resource<Controls>(c);
    // run_if: only worlds holding a Run (not a pause menu's world, if you add one).
    app.add_system({.stage = lam::Stage::Simulate, .fn = play, .name = "grid_puzzle.play",
                    .run_if = &lam::has_local<Run>});
}

}  // namespace grid_puzzle
