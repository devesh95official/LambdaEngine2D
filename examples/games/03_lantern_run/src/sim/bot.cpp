#include "sim/bot.h"

#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/steering.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/path_follow.h>

#include "sim/rules.h"

namespace lantern {

void Autopilot::fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) {
    lam::InputFrame &out = frames[0];
    out.clear_devices();                                        // replaces the human, keeps queued intents
    lam::World &w = ctx.world();
    const RunState *run = w.resources().try_get<RunState>();
    const Tuning *t = ctx.try_res<Tuning>();
    const lam::TileWorld *tiles = w.resources().try_get<lam::TileWorld>();
    if (run == nullptr || t == nullptr || tiles == nullptr) return;

    if (run->phase == Phase::Title) {
        out.press(controls_.confirm);
        return;
    }
    if (run->phase == Phase::Over) {
        if (w.tick() > run->over_at + static_cast<std::uint64_t>(t->restart_after_ticks)) out.press(controls_.confirm);
        return;
    }
    lam::Entity pe;
    const Player *player = w.single_or_null<Player>(&pe);
    if (player == nullptr || !player->alive) return;
    const lam::Vec2 feet = w.get<lam::Transform>(pe).position;

    // Danger first: a wolf winding up close by.
    for (auto [e, wolf, tr] : w.view<Wolf, lam::Transform>()) {
        (void)e;
        const bool winding = wolf.fsm.in(WolfState::Telegraph) || wolf.fsm.in(WolfState::Lunge);
        if (!winding || lam::distance(tr.position, feet) >= 70.0f) continue;
        const lam::Vec2 away = lam::normalize_or(feet - tr.position, {1.0f, 0.0f});
        // Sideways is better than straight away: the lunge is along a locked line.
        out.set_axis(controls_.move, clamp_input(lam::perp(away) + away * 0.5f));
        // Ticks left in the wind-up: telegraph + 1 - ticks_in_state.
        const int left = t->wolf_telegraph + 1 - static_cast<int>(wolf.fsm.ticks_in_state);
        if (wolf.fsm.in(WolfState::Telegraph) && left <= 10) out.press(controls_.dash);
        return;
    }

    // Then the nearest lantern.
    bool found = false;
    lam::Vec2 best;
    for (auto [e, l, tr] : w.view<Lantern, lam::Transform>()) {
        (void)e;
        if (l.present && (!found || lam::distance(tr.position, feet) < lam::distance(best, feet))) {
            best = tr.position;
            found = true;
        }
    }
    if (!found) return;
    if (lam::box_path_clear(*tiles, foot_box(t->player_box), feet, best)) {
        out.set_axis(controls_.move, lam::normalize_or(best - feet, {}));
        return;
    }
    const int cells = tiles->cost.width() * tiles->cost.height();
    if (cells > sized_cells_) {
        paths_.resize(cells);
        sized_cells_ = cells;
    }
    const lam::IVec2 goal = standing_tile(*tiles, best, t->player_box);
    if (lam::route_stale(route_, goal, w.tick(), route_tick_, 20)) {
        paths_.begin_tick(1);
        const lam::IVec2 from = standing_tile(*tiles, feet, t->player_box);
        (void)paths_.find(tiles->cost, from, goal, lam::GridDirs::Eight, route_);
        route_tick_ = w.tick();
    }
    out.set_axis(controls_.move, lam::follow_route(route_, *tiles, body_at(feet, t->player_box).center(), 3.0f));
}

} // namespace lantern
