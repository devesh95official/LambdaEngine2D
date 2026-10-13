// The --bot driver, also used by the test. Stateless: every choice comes from world state.
// TODO(bot): teach it your new rules, so `--headless --bot` keeps testing the whole game.
#include <game/game.h>

#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>

namespace game {
using namespace lam;

void Bot::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    World &w = ctx.world();
    const Controls *c = ctx.try_res<Controls>();
    const Run *run = w.resources().try_get<Run>();
    const Entity me = find_player(w);
    if (c == nullptr || run == nullptr || !me) return;          // not the game world
    InputFrame &f = frames[0];                                  // edit in place, keeps UI intents
    f.pointer_valid = false;
    const auto press_fire = [&] {
        const std::uint64_t bit = std::uint64_t{1} << (c->fire.id & 63u);
        f.pressed[c->fire.id >> 6] |= bit;
        f.held[c->fire.id >> 6] |= bit;
    };
    if (run->over) {                                            // restart after a second
        if (w.tick() % 60 == 0) press_fire();
        return;
    }
    // Shoot the nearest enemy. Back away from close ones, strafe (switching side every 3 s) and
    // drift toward the centre.
    const Vec2 p = w.get<Transform>(me).position;
    Vec2 nearest{}, flee{};
    float best = 1e12f;
    for (auto [e, t, enemy] : w.view<Transform, Enemy>()) {
        const Vec2 d = t.position - p;
        const float d2 = length_sq(d);
        if (d2 < best) {
            best = d2;
            nearest = t.position;
        }
        if (d2 < 90.0f * 90.0f) flee -= d / (d2 + 1.0f);
    }
    if (best < 1e12f) {
        f.pointer_world = nearest + Vec2{0, -5};                // aim at the body, not the feet
        f.pointer_valid = true;
        press_fire();
    }
    const Vec2 away = normalize_or(flee);
    const float side = (w.tick() / 180) % 2 == 0 ? 1.0f : -1.0f;
    const Vec2 center = cell_center({kArenaW / 2, kArenaH / 2});
    const Vec2 home = distance_sq(center, p) > 48.0f * 48.0f ? normalize_or(center - p) : Vec2{};
    f.axes[c->move.id] = clamp_length(away + perp(away) * (0.6f * side) + home * 0.4f, 1.0f);
}
}
