// The turn loop: lam::next_actor hands out turns by energy; the
// player's turn waits for input, monsters act at once. While any GridMover animates, grid_movers
// (Animate -1200) sets TurnScheduler::busy and next_actor returns null, so the world pauses until
// steps finish; attacks do not animate, so a fight resolves at one exchange per tick. Steps
// outside the hero's view are not animated (monster_step), so only visible motion pauses turns.
#include <lam/audio/components.h>
#include <lam/ecs/turns.h>
#include <lam/input/input_frame.h>
#include <lam/tilemap/grid_mover.h>
#include <optional>
#include <sim/controls.h>
#include <sim/rules.h>
#include <sim/run.h>

namespace rl {
namespace {
constexpr lam::IVec2 kDirs[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};

lam::IVec2 direction_pressed(const Controls &c, const lam::InputFrame &in) {
    if (in.is_repeated(c.up)) return {0, -1}; // is_repeated: press + key repeat
    if (in.is_repeated(c.down)) return {0, 1};
    if (in.is_repeated(c.left)) return {-1, 0};
    if (in.is_repeated(c.right)) return {1, 0};
    return {};
}

// A monster step: one nobody can see (both cells outside the FOV) finishes at once, so monsters
// shuffling in the dark never pause the turn loop (grid_movers keeps TurnScheduler::busy while
// any GridMover animates; elapsed = ticks completes the step in this tick's Animate pass).
lam::StepResult monster_step(lam::World &w, lam::Entity e, lam::IVec2 dir,
                             lam::Entity *blocker = nullptr) {
    const lam::IVec2 from = w.get<lam::GridPosition>(e).cell;
    const lam::StepResult r = lam::try_step(w, e, dir, {}, blocker);
    if (r == lam::StepResult::Moved && !cell_visible(w, from) && !cell_visible(w, from + dir))
        if (lam::GridMover *mover = w.try_get<lam::GridMover>(e)) mover->elapsed = mover->ticks;
    return r;
}

bool monster_in_view(lam::World &w) {
    for (auto [e, m, pos] : w.view<Monster, lam::GridPosition>()) {
        (void)e;
        (void)m;
        if (cell_visible(w, pos.cell)) return true;
    }
    return false;
}

// The player's action for this tick; returns its energy cost (0 = no action, keep waiting).
std::int32_t player_act(const lam::Ctx &ctx, lam::Entity player, const lam::InputFrame &in) {
    lam::World &w = ctx.world();
    RunState &run = ctx.local<RunState>();
    const Controls *c = ctx.try_res<Controls>();
    if (!c) return 0;
    if (run.pending_use >= 0) { // UI decision (kUseItem intent), kept by take_turns
        const int slot = run.pending_use;
        run.pending_use = -1;
        if (const std::int32_t cost = use_item(ctx, player, slot)) return cost;
    }

    const lam::IVec2 cell = w.get<lam::GridPosition>(player).cell;
    if (in.is_pressed(c->descend)) {
        if (cell != run.stairs) {
            say(w, "There are no stairs here.", lam::Color{170, 170, 170, 255});
            return 0;
        }
        ++run.depth;
        run.score += 50;
        run.phase = Phase::NeedFloor; // generate_floor runs next tick
        say(w, "You take the stairs down.", lam::Color{160, 200, 255, 255});
        ctx.emit(lam::PlaySound{.group = "stairs"_id});
        return kTurnCost;
    }
    if (in.is_repeated(c->wait)) {
        Stats &st = w.get<Stats>(player); // resting heals when nothing is in view
        if (!monster_in_view(w) && run.floor_turns % 2 == 0 && st.hp < st.max_hp) ++st.hp;
        return kTurnCost;
    }
    const lam::IVec2 dir = direction_pressed(*c, in);
    if (dir == lam::IVec2{}) return 0;
    lam::Entity blocker;
    switch (lam::try_step(w, player, dir, {}, &blocker)) {
    case lam::StepResult::Moved: // GridMover animates; scheduler goes busy
        pick_up_items(ctx, player, cell + dir);
        if (cell + dir == run.stairs)
            say(w, "Stairs lead down here. Press . or Enter.", lam::Color{250, 220, 120, 255});
        refresh_fov(w);
        return kTurnCost;
    case lam::StepResult::BlockedEntity: // bump-to-attack (GridIndex found it)
        if (w.alive(blocker) && w.has<Monster>(blocker)) {
            attack(ctx, player, blocker);
            return kTurnCost;
        }
        return 0;
    case lam::StepResult::BlockedTile:
    case lam::StepResult::Busy:
        return 0;
    }
    return 0;
}

void monster_act(const lam::Ctx &ctx, lam::Entity e) {
    lam::World &w = ctx.world();
    const RunState &run = ctx.local<RunState>();
    if (!w.alive(run.player)) return;
    const lam::IVec2 cell = w.get<lam::GridPosition>(e).cell;
    const lam::IVec2 target = w.get<lam::GridPosition>(run.player).cell;
    if (!w.get<Monster>(e).awake) { // asleep: shuffle about now and then
        lam::Rng &rng = ctx.rng("ai"_id);
        if (rng.below(4) == 0) (void)monster_step(w, e, kDirs[rng.below(4)]);
        return;
    }
    if (lam::manhattan(cell, target) == 1) {
        attack(ctx, e, run.player);
        return;
    }
    // Walk down the Dijkstra map toward the player (shared by every monster; recomputed only
    // when the player moved or the tiles changed).
    const std::optional<lam::IVec2> next = chase_map(w, target).map.step_toward_goal(cell);
    if (!next) return;
    lam::IVec2 dir = *next - cell;
    dir = {dir.x > 0 ? 1 : dir.x < 0 ? -1 : 0, dir.y > 0 ? 1 : dir.y < 0 ? -1 : 0};
    lam::Entity blocker;
    (void)monster_step(w, e, dir, &blocker); // blocked by another monster: wait
}
} // namespace

void take_turns(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    RunState &run = ctx.local<RunState>();
    if (run.phase != Phase::Playing) return;
    const lam::InputFrame &in = lam::input_frame(ctx); // neutral unless this world is focused
    // UI decisions arrive as intents, delivered in exactly one tick's InputFrame. That tick may be
    // a busy one (steps animating, next_actor returns null), so remember the choice in RunState.
    for (const lam::Intent &intent : in.intent_list())
        if (intent.kind == kUseItem && intent.a >= 0 && intent.a < kInventorySlots)
            run.pending_use = static_cast<std::int8_t>(intent.a);
    bool input_used = false;
    // next_actor: the actor whose turn it is, granting energy rounds as needed; null while busy
    // (animations), when the per-tick budget is spent, or when nobody can act.
    for (lam::Entity e; (e = lam::next_actor(w));) {
        if (!w.alive(e) || !w.has<Stats>(e)) { // defensive: an actor without stats
            lam::end_turn(w, e);
            continue;
        }
        if (w.has<lam::PlayerTurn>(e)) {
            if (input_used) break; // one input per tick
            input_used = true;
            const std::int32_t cost = player_act(ctx, e, in);
            if (cost == 0) break; // still the player's turn next tick
            lam::end_turn(w, e, cost);
            ++run.floor_turns;
            ++run.turns;
            if (run.phase != Phase::Playing) break; // died or took the stairs
        } else {
            monster_act(ctx, e);
            lam::end_turn(w, e);
            if (run.phase != Phase::Playing) break;
        }
    }
}
} // namespace rl
