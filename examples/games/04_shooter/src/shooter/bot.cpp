// Autoplay: kite, strafe, collect, aim with lead. Writes only what a player could (axes, intents).
#include <shooter/bot.h>

#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/scalar.h>
#include <lam/physics/components.h>
#include <lam/physics/steering.h>
#include <lam/physics/tile_world.h>
#include <shooter/arena.h>
#include <shooter/components.h>
#include <shooter/ids.h>

namespace shooter {
using namespace lam;

namespace {
constexpr float kThreatRadius = 110;                            // closer enemies push the bot away
constexpr float kFireRange = 260;
constexpr float kPickupRange = 220;
constexpr std::uint64_t kMenuDelayTicks = 45;                   // let a watcher read the menu

bool solid(const TileWorld *tiles, Vec2 p) {
    return tiles && tiles->at(tiles->tile_at(p)) == TileShape::Solid;
}
}

void ShooterBot::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    const Controls *c = ctx.try_res<Controls>();
    if (c == nullptr) return;
    // The frame already holds this tick's live input and queued intents (menu picks, restart):
    // edit it in place with the InputFrame writers, never assign InputFrame{}.
    InputFrame &f = frames[0];
    World &w = ctx.world();                                     // focused world (arena or menu)

    if (!w.resources().has<Arena>()) {                          // a menu scene: choose item 0
        if (w.tick() >= kMenuDelayTicks && w.tick() % kMenuDelayTicks == 0)
            f.push_intent(Intent{ids::kIntentMenuSelect, 0, 0});    // false when full: next time
        return;
    }
    const Entity player = find_player(w);
    if (!player) return;
    const Collider *pc = w.try_get<Collider>(player);
    const Vec2 me = w.get<Transform>(player).position + (pc ? pc->offset : Vec2{});
    const Weapon *wpn = w.try_get<Weapon>(player);
    const float bullet_speed = wpn ? wpn->bullet_speed : 300.0f;

    // Threat field + nearest visible target in one pass (dense order: deterministic).
    // Line of sight uses can_see (a ray over TileWorld's opaque tiles) rather than the physics
    // raycast, so it only depends on the static map.
    const TileWorld *tiles = w.resources().try_get<TileWorld>();
    Vec2 flee{};
    int threats = 0;
    float best = kFireRange * kFireRange;
    Vec2 aim_at{};
    bool has_target = false;
    for (auto [e, t, enemy] : w.view<Transform, Enemy>()) {
        const Vec2 pos = t.position + Vec2{0, -6};
        const Vec2 d = pos - me;
        const float d2 = length_sq(d);
        if (d2 < kThreatRadius * kThreatRadius) {
            const float weight = enemy.kind == EnemyKind::Brute ? 2.0f : 1.0f;
            flee -= d * (weight / (d2 + 1.0f));
            ++threats;
        }
        if (d2 < best && (tiles == nullptr || can_see(*tiles, me, pos, kFireRange))) {
            best = d2;
            const Velocity *v = w.try_get<Velocity>(e);
            const float flight = length(d) / bullet_speed;     // lead the target
            aim_at = pos + (v ? v->linear * flight : Vec2{});
            has_target = true;
        }
    }

    // Movement: away from threats with a strafe that flips every 4 s, a pull toward the centre so
    // it does not get cornered; with no threats, go for pickups or drift to the centre.
    const float strafe = (w.tick() / 240) % 2 == 0 ? 1.0f : -1.0f;
    const Vec2 to_center = Vec2{kArenaW * kTileSize * 0.5f, kArenaH * kTileSize * 0.5f} - me;
    Vec2 move{};
    if (threats > 0) {
        const Vec2 away = normalize_or(flee);
        move = away + perp(away) * (0.7f * strafe) + normalize_or(to_center) * 0.35f;
    } else {
        float pick = kPickupRange * kPickupRange;
        for (auto [e, t, p] : w.view<Transform, Pickup>()) {
            const float d2 = distance_sq(t.position, me);
            if (d2 < pick) {
                pick = d2;
                move = normalize_or(t.position - me);
            }
        }
        if (move == Vec2{} && length_sq(to_center) > 64.0f * 64.0f)
            move = normalize_or(to_center) * 0.6f;
    }
    // Wall avoidance: rotate the heading until the probe 18 px ahead is free.
    if (move != Vec2{}) {
        Vec2 dir = normalize_or(move);
        for (int k = 0; k < 8 && solid(tiles, me + dir * 18.0f); ++k)
            dir = rotate(dir, kPi / 4 * strafe);
        move = dir * clamp(length(move), 0.0f, 1.0f);
    }

    f.clear_devices();                                          // the bot replaces the human (keeps intents)
    f.set_axis(c->move, move);
    // Aim stick deflected = aim + fire (twin-stick); zero = hold fire.
    f.set_axis(c->aim, has_target ? normalize_or(aim_at - me) : Vec2{});
}
}
