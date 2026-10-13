// Wave director: intermission countdown -> wave start -> a per-tick spawn budget from the
// arena's spawn pads -> wave cleared. Spawns are prefabs recorded through Commands, so hundreds
// of enemies arrive over a few seconds without a frame spike or a structural change mid-view.
#include <shooter/rules.h>

#include <algorithm>
#include <array>
#include <lam/audio/components.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/rng.h>
#include <shooter/arena.h>
#include <shooter/components.h>
#include <shooter/ids.h>

namespace shooter {
using namespace lam;

namespace {
constexpr std::uint32_t kIntermissionTicks = 150;
constexpr float kMinSpawnDistance = 110;                       // never spawn on top of the player
// Prefab Transforms are the feet; the bodies (Collider::offset y = -6, brute -9) sit above them.
// Spawning feet at pad centre + 6 centres the body on the pad. Pads are a tile away from walls, so
// pad + jitter never starts a body inside a wall tile: move_box freezes a box that starts
// overlapping a solid tile on both axes (it does not push it out), so such an enemy never moves.
constexpr Vec2 kPadFeet{0, 6};

// The 'S' cells of the layout, computed once at compile time.
constexpr std::size_t kMaxPads = 16;
struct Pads {
    std::array<IVec2, kMaxPads> cells{};
    std::size_t count = 0;
};
constexpr Pads find_pads() {
    Pads p;
    for (int y = 0; y < kArenaH; ++y)
        for (int x = 0; x < kArenaW; ++x)
            if (kArenaRows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] == 'S' &&
                p.count < kMaxPads)
                p.cells[p.count++] = IVec2{x, y};
    return p;
}
constexpr Pads kPads = find_pads();
static_assert(kPads.count > 0, "the arena needs at least one 'S' spawn pad");

StrId pick_kind(Rng &rng, std::uint32_t wave) {
    const float r = rng.unit();
    const float w = static_cast<float>(wave);
    const float brute = wave >= 3 ? std::min(0.04f + 0.01f * w, 0.12f) : 0.0f;
    const float runner = wave >= 2 ? 0.30f : 0.0f;
    if (r < brute) return ids::kPrefabBrute;
    if (r < brute + runner) return ids::kPrefabRunner;
    return ids::kPrefabGrunt;
}

void spawn_some(Ctx &ctx, Arena &a) {
    World &w = ctx.world();
    const Entity player = find_player(w);
    const Vec2 avoid = player ? w.get<Transform>(player).position : Vec2{-1e6f, -1e6f};
    const std::uint32_t budget = a.stress_enemies > 0
                                     ? std::max<std::uint32_t>(8, a.stress_enemies / 60)
                                     : std::min<std::uint32_t>(2 + a.wave / 2, 10);
    Rng &rng = ctx.rng(ids::kRngSpawn);                         // tick stages only (LAM_ASSERT)
    for (std::uint32_t i = 0; i < budget && a.to_spawn > 0; ++i) {
        // Round-robin over the pads, skipping the ones near the player (at most one full lap).
        Vec2 pad{};
        bool found = false;
        for (std::size_t tries = 0; tries < kPads.count && !found; ++tries) {
            const IVec2 c = kPads.cells[a.spawn_cursor++ % kPads.count];
            pad = tile_center(c.x, c.y);
            found = distance_sq(pad, avoid) > kMinSpawnDistance * kMinSpawnDistance;
        }
        if (!found) return;
        ctx.commands().spawn_prefab(pick_kind(rng, a.wave), pad + kPadFeet + rng.in_disc(6.0f));
        --a.to_spawn;
    }
}
}

void wave_director(Ctx &ctx) {
    Arena &a = ctx.local<Arena>();
    World &w = ctx.world();
    switch (a.phase) {
    case Phase::Intermission:
        if (a.phase_ticks > 0) {
            --a.phase_ticks;
            return;
        }
        ++a.wave;
        a.to_spawn = wave_size(a.wave, a.stress_enemies);
        a.phase = Phase::Fighting;
        ctx.emit(WaveStarted{a.wave, a.to_spawn});
        ctx.emit(PlaySound{ids::kSndWave, {}, false, 1.0f});
        return;
    case Phase::Fighting:
        // Check before spawning: count<Enemy>() sees only applied spawns (Commands apply after
        // the stage), so this tick's spawns cannot make an unfinished wave look cleared.
        if (a.to_spawn == 0 && w.count<Enemy>() == 0) {
            const std::int64_t bonus = 50 * static_cast<std::int64_t>(a.wave);
            a.score += bonus;
            a.phase = Phase::Intermission;
            a.phase_ticks = kIntermissionTicks;
            ctx.emit(WaveCleared{a.wave, bonus});
            return;
        }
        spawn_some(ctx, a);
        return;
    case Phase::GameOver: return;
    }
}
}
