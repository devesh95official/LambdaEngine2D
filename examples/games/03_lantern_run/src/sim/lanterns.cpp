// Lanterns: walking over one collects it; it is lit again somewhere else, and every
// lanterns_per_wave lanterns another wolf joins and the pack gets faster (wave). Runs in
// PostPhysics, after the motor moved the traveller.
#include <algorithm>
#include <array>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>

#include "sim/rules.h"

namespace lantern {

void lantern_pickups(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const Tuning &t = tuning(ctx);
    lam::Entity pe;
    if (w.single_or_null<Player>(&pe) == nullptr) return;
    const lam::Vec2 feet = w.get<lam::Transform>(pe).position;
    RunState &run = ctx.local<RunState>();

    // Collect first, then act: place_lantern teleports and spawn_wolf creates entities, which
    // must not happen while a View over those pools is alive.
    std::array<lam::Entity, 32> lanterns{};
    std::size_t n = 0;
    for (auto [e, l] : w.view<Lantern>()) {
        (void)l;
        if (n < lanterns.size()) lanterns[n++] = e;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const lam::Entity e = lanterns[i];
        if (!w.get<Lantern>(e).present) continue;
        const lam::Vec2 at = w.get<lam::Transform>(e).position;
        if (lam::distance(at, feet) >= t.pickup_radius) continue;
        ++run.collected;
        run.best = std::max(run.best, run.collected);
        ctx.emit(LanternCollected{at, run.collected});
        place_lantern(ctx, e);
        if (run.collected % t.lanterns_per_wave == 0) {
            ++run.wave;
            spawn_wolf(ctx);
            ctx.emit(WaveUp{run.wave});
        }
    }
}

} // namespace lantern
