// Energy turn scheduler. Integer maths only; ties go to the lower entity index so pool layout
// never affects the order.
#include <lam/ecs/turns.h>

#include <cstdint>
#include <limits>
#include <span>

#include <lam/core/assert.h>
#include <lam/ecs/world.h>

namespace lam {

namespace {

std::int32_t clamp_energy(std::int64_t v) {
    constexpr std::int64_t lo = std::numeric_limits<std::int32_t>::min();
    constexpr std::int64_t hi = std::numeric_limits<std::int32_t>::max();
    return static_cast<std::int32_t>(v < lo ? lo : (v > hi ? hi : v));
}

TurnScheduler *scheduler(World &w) {
    TurnScheduler *s = w.resources().try_get<TurnScheduler>();
    LAM_ASSERT(s != nullptr, "turn scheduler missing: call add_turn_scheduler(world) first");
    return s;
}

void roll_tick(World &w, TurnScheduler &s) {
    if (s.last_tick != w.tick()) {
        s.last_tick = w.tick();
        s.acted_this_tick = 0;
    }
}

} // namespace

TurnScheduler &add_turn_scheduler(World &w) {
    TurnScheduler *s = w.resources().try_get<TurnScheduler>();
    if (!s) s = &w.resources().insert<TurnScheduler>();
    w.snapshot_resource<TurnScheduler>();
    return *s;
}

Entity next_actor(World &w) {
    TurnScheduler *sp = scheduler(w);
    if (!sp) return {};
    TurnScheduler &s = *sp;
    roll_tick(w, s);
    if (s.busy) return {};
    if (s.current) {
        if (w.alive(s.current) && w.has<TurnActor>(s.current)) return s.current;
        s.current = {};
    }
    if (s.acted_this_tick >= s.max_actors_per_tick) return {};
    for (int pass = 0; pass < 2; ++pass) {
        Entity best;
        std::int32_t best_energy = 0;
        std::int64_t rounds = std::numeric_limits<std::int64_t>::max();
        w.view<TurnActor>().each([&](Entity e, TurnActor &a) {
            if (a.energy >= kTurnThreshold && (!best || a.energy > best_energy ||
                                               (a.energy == best_energy && e.index < best.index))) {
                best = e;
                best_energy = a.energy;
            }
            if (a.speed > 0) {
                const std::int64_t need = std::int64_t(kTurnThreshold) - a.energy;
                const std::int64_t r = need <= 0 ? 0 : (need + a.speed - 1) / a.speed;
                if (r < rounds) rounds = r;
            }
        });
        if (best) {
            s.current = best;
            return best;
        }
        if (rounds == std::numeric_limits<std::int64_t>::max() || pass == 1) return {};
        // Grant the fewest whole rounds of energy after which someone can act.
        w.view<TurnActor>().each([&](Entity, TurnActor &a) {
            if (a.speed > 0)
                a.energy = clamp_energy(std::int64_t(a.energy) + std::int64_t(a.speed) * rounds);
        });
    }
    return {};
}

Entity peek_actor(const World &w) {
    const TurnScheduler *s = w.resources().try_get<TurnScheduler>();
    if (!s || s->busy) return {};
    if (s->current && w.alive(s->current) && w.has<TurnActor>(s->current)) return s->current;
    // next_actor() rolls the per-tick counter first.
    const std::uint16_t acted = s->last_tick == w.tick() ? s->acted_this_tick : std::uint16_t{0};
    if (acted >= s->max_actors_per_tick) return {};
    const std::span<const Entity> actors =
        detail::pool_entities(w, ComponentRegistry::global().id<TurnActor>());
    // Same two passes as next_actor(); the second one on the energies it would grant.
    std::int64_t rounds = std::numeric_limits<std::int64_t>::max();
    for (int pass = 0; pass < 2; ++pass) {
        Entity best;
        std::int32_t best_energy = 0;
        for (const Entity e : actors) {
            const TurnActor &a = *w.try_get<TurnActor>(e);
            std::int32_t energy = a.energy;
            if (pass == 1 && a.speed > 0)
                energy = clamp_energy(std::int64_t(a.energy) + std::int64_t(a.speed) * rounds);
            if (energy >= kTurnThreshold &&
                (!best || energy > best_energy || (energy == best_energy && e.index < best.index))) {
                best = e;
                best_energy = energy;
            }
            if (pass == 0 && a.speed > 0) {
                const std::int64_t need = std::int64_t(kTurnThreshold) - a.energy;
                const std::int64_t r = need <= 0 ? 0 : (need + a.speed - 1) / a.speed;
                if (r < rounds) rounds = r;
            }
        }
        if (best) return best;
        if (rounds == std::numeric_limits<std::int64_t>::max()) return {};
    }
    return {};
}

void end_turn(World &w, Entity actor, std::int32_t cost) {
    TurnScheduler *sp = scheduler(w);
    if (TurnActor *a = w.try_get<TurnActor>(actor))
        a->energy = clamp_energy(std::int64_t(a->energy) - cost);
    if (!sp) return;
    roll_tick(w, *sp);
    if (sp->current == actor) sp->current = {};
    ++sp->turns;
    if (sp->acted_this_tick < 0xFFFFu) ++sp->acted_this_tick;
}

void set_turns_busy(World &w, bool busy) {
    if (TurnScheduler *s = scheduler(w)) s->busy = busy;
}

} // namespace lam
