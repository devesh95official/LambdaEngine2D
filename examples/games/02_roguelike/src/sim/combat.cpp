// Melee and damage. Rolls come from the world stream "combat", so loads and replays roll the same.
// Feedback goes out as Hit and PlaySound events.
#include <lam/audio/components.h>
#include <lam/ecs/prefab.h>
#include <lam/tilemap/grid_mover.h>
#include <sim/rules.h>
#include <sim/run.h>

namespace rl {
namespace {
// PlaySound is registered by RulesPlugin too (add_event is idempotent), so the rules emit it
// with or without AudioPlugin (headless tests).
void play(const lam::Ctx &ctx, lam::StrId group, lam::Vec2 at) {
    ctx.emit(lam::PlaySound{.group = group, .position = at});
}

lam::FixedString<16> name_of(const lam::World &w, lam::Entity e) {
    if (const Monster *m = w.try_get<Monster>(e)) return m->name;
    return lam::FixedString<16>("you");
}

void gain_xp(const lam::Ctx &ctx, lam::Entity player, std::uint16_t xp) {
    lam::World &w = ctx.world();
    Stats *st = w.try_get<Stats>(player);
    if (!st) return;
    st->xp = static_cast<std::uint16_t>(st->xp + xp);
    // Level L -> L + 1 at 10 * L + 5 * L * (L - 1) / 2 xp (10, 25, 45, 70, 100, ...): each level
    // costs 5 xp more than the last, so the hero does not outgrow every floor (cap 20).
    while (st->level < 20 && st->xp >= 10 * st->level + 5 * st->level * (st->level - 1) / 2) {
        // +4 max hp and +1 attack per level, +1 defense every third level
        ++st->level;
        st->max_hp = static_cast<std::int16_t>(st->max_hp + 4);
        st->hp = static_cast<std::int16_t>(st->hp + 4);
        st->attack = static_cast<std::int16_t>(st->attack + 1);
        if (st->level % 3 == 0) st->defense = static_cast<std::int16_t>(st->defense + 1);
        lam::FixedString<96> line;
        lam::format_to(line, "You reach level {}!", st->level);
        say(w, line.view(), lam::Color{255, 240, 120, 255});
    }
}
} // namespace

void attack(const lam::Ctx &ctx, lam::Entity attacker, lam::Entity target) {
    lam::World &w = ctx.world();
    const Stats *a = w.try_get<Stats>(attacker);
    const Stats *d = w.try_get<Stats>(target);
    if (!a || !d) return;
    lam::Rng &rng = ctx.rng("combat"_id);
    if (rng.below(10) == 0) { // 10% miss
        lam::FixedString<96> line;
        const bool player_attacks = w.has<Player>(attacker);
        if (player_attacks)
            lam::format_to(line, "You miss the {}.", name_of(w, target).view());
        else
            lam::format_to(line, "The {} misses you.", name_of(w, attacker).view());
        say(w, line.view(), lam::Color{170, 170, 170, 255});
        return;
    }
    const int roll = a->attack - d->defense + rng.between(-1, 1);
    damage(ctx, attacker, target, roll < 1 ? 1 : roll);
}

void damage(const lam::Ctx &ctx, lam::Entity attacker, lam::Entity target, int amount) {
    lam::World &w = ctx.world();
    RunState &run = ctx.local<RunState>();
    Stats *st = w.try_get<Stats>(target);
    const lam::GridPosition *pos = w.try_get<lam::GridPosition>(target);
    if (!st || !pos) return;
    st->hp = static_cast<std::int16_t>(st->hp - amount);
    const bool killed = st->hp <= 0;
    const lam::IVec2 cell = pos->cell;
    const lam::Vec2 at = w.resources().get<lam::TileWorld>().tile_center(cell);
    ctx.emit(Hit{.attacker = attacker,
                 .target = target,
                 .cell = cell,
                 .damage = static_cast<std::int16_t>(amount),
                 .killed = killed});
    play(ctx, killed ? "kill"_id : "hit"_id, at);

    lam::FixedString<96> line;
    if (target == run.player) {
        lam::format_to(line, "The {} hits you for {}.", name_of(w, attacker).view(), amount);
        say(w, line.view(), lam::Color{255, 110, 100, 255});
        if (killed) {
            run.phase = Phase::Dead; // permadeath: the run ends here
            say(w, "You die...", lam::Color{255, 60, 60, 255});
            play(ctx, "death"_id, at);
        }
        return;
    }
    const lam::FixedString<16> name = name_of(w, target);
    lam::format_to(line, "You hit the {} for {}.", name.view(), amount);
    say(w, line.view());
    if (!killed) return;
    const std::uint16_t xp = st->xp;
    ++run.kills;
    run.score += xp * 10u;
    lam::format_to(line, "The {} dies.", name.view());
    say(w, line.view(), lam::Color{200, 255, 160, 255});
    w.destroy(target); // immediate: no View is alive here
    const lam::PrefabLibrary &prefabs = ctx.res<lam::PrefabLibrary>();
    if (prefabs.has("corpse"_id)) {
        const lam::Entity corpse = prefabs.spawn(w, "corpse"_id, at);
        lam::set_grid_position(w, corpse, cell, true);
    }
    if (w.alive(run.player)) gain_xp(ctx, run.player, xp);
}
} // namespace rl
