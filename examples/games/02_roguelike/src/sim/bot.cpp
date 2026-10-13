#include <sim/bot.h>

#include <lam/ecs/turns.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_mover.h>
#include <optional>
#include <sim/controls.h>
#include <sim/data.h>
#include <sim/run.h>

namespace rl {
namespace {
lam::ButtonAction action_for(const Controls &c, lam::IVec2 dir) {
    if (dir.y < 0) return c.up;
    if (dir.y > 0) return c.down;
    if (dir.x < 0) return c.left;
    if (dir.x > 0) return c.right;
    return c.wait;
}

int slot_with(const Inventory &inv, const GameData &data, ItemEffect effect) {
    for (int i = 0; i < inv.count; ++i)
        if (const ItemDef *def = data.items.find(inv.slots[static_cast<std::size_t>(i)]);
            def && def->effect == effect)
            return i;
    return -1;
}

bool walkable(const lam::TileWorld &tiles, lam::IVec2 c) {
    return tiles.at(c) != lam::TileShape::Solid;
}
} // namespace

bool BotDriver::route(const lam::World &w, lam::IVec2 from) {
    const lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    const int width = tiles.shapes.width(), height = tiles.shapes.height();
    if (cost_.width() != width || cost_.height() != height) {
        cost_.resize(width, height, 0); // per map size, not per decision
        map_.resize({width, height});
    }
    for (int y = 0; y < height; ++y) // only through cells the hero has seen
        for (int x = 0; x < width; ++x)
            cost_(x, y) = cell_explored(w, {x, y}) && walkable(tiles, {x, y}) ? std::uint16_t{10}
                                                                              : std::uint16_t{0};
    cost_(from.x, from.y) = 10;
    map_.compute(cost_, goals_, lam::GridDirs::Four);
    return map_.at(from) != UINT32_MAX;
}

void BotDriver::fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) {
    lam::World &w = ctx.world(); // the focused world (menus: no RunState)
    const RunState *run = run_state(w);
    const lam::TurnScheduler *turns = w.resources().try_get<lam::TurnScheduler>();
    const Controls *c = ctx.try_res<Controls>();
    const GameData *data = ctx.try_res<GameData>();
    lam::InputFrame &f = frames[0];
    if (!run) { // a menu has focus: the game-over menu if a dead run lies below it
        // The death and the menu push happen in the same tick and the push applies at the next
        // tick boundary (before fill), so the bot never sees the dead run focused: look below.
        if (restart_.kind)
            for (const lam::WorldEntry &entry : ctx.app().world_stack())
                if (const RunState *below = entry.world ? run_state(*entry.world) : nullptr;
                    below && below->phase == Phase::Dead)
                    return (void)f.push_intent(restart_);
        return;
    }
    if (run->phase != Phase::Playing) return;
    if (!turns || turns->busy || !c || !data) return;
    if (!w.alive(run->player) || !w.resources().has<lam::TileWorld>()) return;
    const lam::IVec2 me = w.get<lam::GridPosition>(run->player).cell;
    const Stats &st = w.get<Stats>(run->player);
    const Inventory &inv = w.get<Inventory>(run->player);
    ++decisions_;

    // 1. Low on health: drink.
    if (st.hp * 5 < st.max_hp * 2)
        if (const int slot = slot_with(inv, *data, ItemEffect::Heal); slot >= 0)
            return (void)f.push_intent(lam::Intent{.kind = kUseItem, .a = slot});
    // 2. A monster next to us: zap it if we are hurt, else bump into it (bump-to-attack).
    lam::Entity foe;
    lam::IVec2 foe_cell;
    for (auto [e, m, pos] : w.view<Monster, lam::GridPosition>()) {
        (void)m;
        if (lam::manhattan(pos.cell, me) == 1 && (!foe || e.index < foe.index))
            foe = e, foe_cell = pos.cell;
    }
    if (foe) {
        if (st.hp * 2 < st.max_hp)
            if (const int slot = slot_with(inv, *data, ItemEffect::Lightning); slot >= 0)
                return (void)f.push_intent(lam::Intent{.kind = kUseItem, .a = slot});
        return f.press(action_for(*c, foe_cell - me));
    }
    // 3. Hurt and nothing in sight: rest (it heals only while no monster is in view).
    if (st.hp * 3 < st.max_hp * 2) {
        bool seen = false;
        for (auto [e, m, pos] : w.view<Monster, lam::GridPosition>()) {
            (void)e;
            (void)m;
            seen = seen || cell_visible(w, pos.cell);
        }
        if (!seen) return f.press(c->wait);
    }
    // 4. A mapping scroll is best read before exploring.
    if (!cell_explored(w, run->stairs))
        if (const int slot = slot_with(inv, *data, ItemEffect::Mapping); slot >= 0)
            return (void)f.push_intent(lam::Intent{.kind = kUseItem, .a = slot});

    // 5. Goals: known items, else the exploration frontier, else (or after lingering) the stairs.
    const lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    goals_.clear();
    if (inv.count < kInventorySlots)
        for (auto [e, item, pos] : w.view<Item, lam::GridPosition>()) {
            (void)e;
            (void)item;
            if (cell_explored(w, pos.cell)) goals_.push_back(pos.cell);
        }
    bool reachable = !goals_.empty() && route(w, me);
    if (!reachable) { // no reachable item: walk to the nearest explored cell next to the unknown
        goals_.clear();
        for (int y = 1; y + 1 < tiles.shapes.height(); ++y)
            for (int x = 1; x + 1 < tiles.shapes.width(); ++x) {
                const lam::IVec2 p{x, y};
                if (!cell_explored(w, p) || !walkable(tiles, p)) continue;
                if (!cell_explored(w, {x + 1, y}) || !cell_explored(w, {x - 1, y}) ||
                    !cell_explored(w, {x, y + 1}) || !cell_explored(w, {x, y - 1}))
                    goals_.push_back(p);
            }
        reachable = !goals_.empty() && route(w, me);
    }
    const bool stairs_known = cell_explored(w, run->stairs);
    if (stairs_known && (!reachable || run->floor_turns > 300)) {
        if (me == run->stairs) return f.press(c->descend);
        goals_.assign(1, run->stairs);
        reachable = route(w, me);
    }
    if (reachable)
        if (const std::optional<lam::IVec2> next = map_.step_toward_goal(me); next && *next != me)
            return f.press(action_for(*c, *next - me));
    f.press(me == run->stairs ? c->descend : c->wait); // nothing better: rest (heals)
}
} // namespace rl
