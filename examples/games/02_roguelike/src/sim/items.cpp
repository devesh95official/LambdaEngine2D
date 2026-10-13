// Items: picked up automatically when the player steps on them, used through the kUseItem intent
// (HUD button, number key or bot), with effects defined by GameData::items (data/items.json).
#include <lam/audio/components.h>
#include <lam/tilemap/fov.h>
#include <lam/tilemap/grid_mover.h>
#include <sim/data.h>
#include <sim/rules.h>
#include <sim/run.h>

namespace rl {
namespace {
constexpr std::size_t kMaxPerCell = 8;

lam::Entity nearest_visible_monster(lam::World &w, lam::IVec2 from) {
    lam::Entity best;
    int best_dist = 1 << 30;
    for (auto [e, m, pos] : w.view<Monster, lam::GridPosition>()) {
        (void)m;
        const int d = lam::manhattan(pos.cell, from);
        // Ties: lower entity index, so the choice never depends on pool order.
        if (!cell_visible(w, pos.cell) || d > best_dist ||
            (d == best_dist && best && e.index > best.index))
            continue;
        best = e;
        best_dist = d;
    }
    return best;
}

bool cell_free(lam::World &w, lam::IVec2 cell) {
    const lam::TileWorld &tiles = w.resources().get<lam::TileWorld>();
    if (tiles.at(cell) == lam::TileShape::Solid) return false;
    // GridIndex answers "who is on this cell" in O(1); it is kept current by try_step and
    // set_grid_position and rebuilt at Simulate -1500. Any occupant (even an item) disqualifies.
    for (lam::Entity e : w.resources().get<lam::GridIndex>().at(cell))
        if (w.alive(e)) return false;
    return true;
}
} // namespace

void pick_up_items(const lam::Ctx &ctx, lam::Entity player, lam::IVec2 cell) {
    lam::World &w = ctx.world();
    const GameData &data = ctx.res<GameData>();
    lam::Entity here[kMaxPerCell];
    std::size_t n = 0;
    for (auto [e, item, pos] : w.view<Item, lam::GridPosition>()) {
        (void)item;
        if (pos.cell == cell && n < kMaxPerCell) here[n++] = e;
    }
    for (std::size_t i = 0; i < n; ++i) { // the View is gone: destroy is allowed
        Inventory *inv = w.try_get<Inventory>(player);
        if (!inv) return;
        if (inv->count >= kInventorySlots) {
            say(w, "Your pack is full.", lam::Color{200, 200, 200, 255});
            return;
        }
        const lam::StrId def = w.get<Item>(here[i]).def;
        inv->slots[inv->count++] = def;
        lam::FixedString<96> line;
        const ItemDef *item_def = data.items.find(def);
        lam::format_to(line, "You pick up a {}.",
                       item_def ? item_def->name.view() : std::string_view("thing"));
        say(w, line.view(), lam::Color{150, 230, 150, 255});
        ctx.emit(lam::PlaySound{.group = "pickup"_id});
        w.destroy(here[i]);
    }
}

std::int32_t use_item(const lam::Ctx &ctx, lam::Entity player, int slot) {
    lam::World &w = ctx.world();
    Inventory *inv = w.try_get<Inventory>(player);
    if (!inv || slot < 0 || slot >= inv->count) return 0;
    const lam::StrId id = inv->slots[static_cast<std::size_t>(slot)];
    for (int i = slot; i + 1 < inv->count; ++i) // consume first: effects may destroy
        inv->slots[static_cast<std::size_t>(i)] = inv->slots[static_cast<std::size_t>(i) + 1];
    inv->slots[--inv->count] = {};
    const ItemDef *def = ctx.res<GameData>().items.find(id);
    if (!def) return 0;
    const lam::IVec2 at = w.get<lam::GridPosition>(player).cell;
    lam::FixedString<96> line;
    switch (def->effect) {
    case ItemEffect::Heal: {
        Stats &st = w.get<Stats>(player);
        const int hp = st.hp + def->power + st.max_hp / 4; // grows with the hero
        st.hp = static_cast<std::int16_t>(hp < st.max_hp ? hp : st.max_hp);
        lam::format_to(line, "You drink the {}. You feel better.", def->name.view());
        say(w, line.view(), lam::Color{150, 230, 150, 255});
        break;
    }
    case ItemEffect::Lightning: {
        const lam::Entity target = nearest_visible_monster(w, at);
        if (!target) {
            say(w, "The scroll crackles, but nothing is in sight.", lam::Color{170, 170, 170, 255});
            break;
        }
        say(w, "Lightning leaps from the scroll!", lam::Color{160, 220, 255, 255});
        damage(ctx, player, target, def->power);
        break;
    }
    case ItemEffect::Mapping: {
        lam::FovMemory &fov = ctx.local<lam::FovMemory>();
        const lam::TileWorld &tiles = ctx.local<lam::TileWorld>();
        for (int y = 0; y < fov.explored.height(); ++y)
            for (int x = 0; x < fov.explored.width(); ++x)
                for (int dy = -1; dy <= 1; ++dy) // floors and the walls around them
                    for (int dx = -1; dx <= 1; ++dx)
                        if (tiles.at({x + dx, y + dy}) != lam::TileShape::Solid)
                            fov.explored(x, y) = 1;
        say(w, "The layout of the floor fills your mind.", lam::Color{160, 220, 255, 255});
        break;
    }
    case ItemEffect::Teleport: {
        const lam::TileWorld &tiles = ctx.local<lam::TileWorld>();
        lam::Rng &rng = ctx.rng("items"_id);
        for (int attempt = 0; attempt < 200; ++attempt) {
            const lam::IVec2 c{rng.between(1, tiles.shapes.width() - 2),
                               rng.between(1, tiles.shapes.height() - 2)};
            if (!cell_free(w, c)) continue;
            lam::set_grid_position(w, player, c,
                                   true); // teleport on the grid (index stays current)
            break;
        }
        refresh_fov(w);
        say(w, "Space folds around you.", lam::Color{200, 160, 255, 255});
        break;
    }
    }
    return kTurnCost;
}
} // namespace rl
