// --bot: every few ticks, aim where the lowest uncovered warhead will be when a counter-missile
// gets there, and fire.
#include <game/game.h>

#include <cmath>
#include <lam/ecs/world.h>

namespace game {
using namespace lam;

void Bot::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    World &w = ctx.world();
    const Controls *c = ctx.try_res<Controls>();
    const Run *r = w.resources().try_get<Run>();
    if (c == nullptr || r == nullptr) return;
    InputFrame &f = frames[0];
    f.clear_devices();
    if (r->phase == kTitle || r->phase == kOver) {
        if (w.tick() % 60 == 30) f.press(c->fire);
        return;
    }
    if (r->phase != kPlay || w.tick() % 8 != 0) return;
    Vec2 aim{};
    float lowest = -1;
    for (auto [e, m] : w.view<Enemy>()) {
        // Same base the rules pick: the nearest one with ammo.
        float from_x = -1, best = 1e9f;
        for (int b = 0; b < kBases; ++b)
            if (ammo(*r, b) > 0 && std::fabs(kBaseX[static_cast<std::size_t>(b)] - m.pos.x) < best) {
                best = std::fabs(kBaseX[static_cast<std::size_t>(b)] - m.pos.x);
                from_x = kBaseX[static_cast<std::size_t>(b)];
            }
        if (from_x < 0) return;                                  // out of ammo
        const float flight = distance(Vec2{from_x, kBaseY}, m.pos) / 6.0f + kBlastGrow * 0.5f;
        const Vec2 p = m.pos + normalize_or(m.to - m.from) * (m.speed * flight);
        if (p.y > kGround - 30 || p.y < 14) continue;
        bool covered = false;
        for (auto [ce, cm] : w.view<Counter>())
            if (distance_sq(cm.to, p) < 20.0f * 20.0f) covered = true;
        for (auto [be, b] : w.view<Blast>())
            if (!b.hostile && b.age < kBlastGrow + kBlastHold && distance_sq(b.pos, p) < 18.0f * 18.0f) covered = true;
        if (!covered && m.pos.y > lowest) {
            lowest = m.pos.y;
            aim = p;
        }
    }
    if (lowest < 0) return;
    f.pointer_world = aim;
    f.pointer_valid = true;
    f.press(c->fire);
}
}
