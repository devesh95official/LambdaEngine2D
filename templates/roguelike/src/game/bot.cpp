// The bot. fill() runs every tick at TickBegin with the focused world. New run after death, hit an
// adjacent monster, else walk the shortest path to the stairs. It reads the whole map, not just
// what the hero has seen. TODO(bot): explore with cell_explored instead.
#include <game/rules.h>

#include <cstdint>
#include <optional>
#include <lam/ecs/turns.h>
#include <lam/ecs/world.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_mover.h>

namespace game {
using namespace lam;

namespace {
// A key press as the rules see it: pressed, held and repeated this tick.
void press(InputFrame &f, ButtonAction a) {
    const std::uint64_t bit = std::uint64_t{1} << (a.id & 63u);
    f.pressed[a.id >> 6] |= bit;
    f.held[a.id >> 6] |= bit;
    f.repeated[a.id >> 6] |= bit;
}

ButtonAction toward(const Controls &c, IVec2 d) {
    return d.y < 0 ? c.up : d.y > 0 ? c.down : d.x < 0 ? c.left : c.right;
}
} // namespace

void BotDriver::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    InputFrame &f = frames[0];
    f.pressed = f.held = f.released = f.repeated = ActionBits{}; // replace the keyboard, keep intents
    World &w = ctx.world();
    const RunState *run = w.resources().try_get<RunState>();
    const TurnScheduler *turns = w.resources().try_get<TurnScheduler>();
    const Controls *c = ctx.try_res<Controls>();
    if (run == nullptr || turns == nullptr || c == nullptr) return;
    if (run->phase == Phase::Dead) return press(f, c->wait); // "Space: new run"
    if (turns->busy || !w.alive(run->player)) return;        // steps still animating
    const IVec2 me = w.get<GridPosition>(run->player).cell;
    for (auto [e, pos] : w.view<GridPosition>(With<FloorLocal>{}))
        if (manhattan(pos.cell, me) == 1) return press(f, toward(*c, pos.cell - me)); // bump = attack
    const TileWorld &tiles = w.resources().get<TileWorld>();
    map_.resize({tiles.cost.width(), tiles.cost.height()});
    const IVec2 goal[1] = {run->stairs};
    map_.compute(tiles.cost, goal, GridDirs::Four);
    const std::optional<IVec2> next = map_.step_toward_goal(me);
    press(f, next ? toward(*c, *next - me) : c->wait);
}
} // namespace game
