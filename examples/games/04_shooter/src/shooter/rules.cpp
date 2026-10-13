// Player, weapon, damage and pickup rules. Wave spawning lives in waves.cpp, crowd steering in
// crowd.cpp. Every structural change goes through ctx.commands() (applied after the stage), so
// it is safe inside view loops.
#include <shooter/rules.h>

#include <algorithm>
#include <array>
#include <lam/audio/components.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
#include <lam/math/scalar.h>
#include <lam/particles/components.h>
#include <lam/physics/components.h>
#include <lam/physics/events.h>
#include <lam/physics/motors.h>
#include <lam/physics/queries.h>
#include <shooter/arena.h>
#include <shooter/components.h>
#include <shooter/ids.h>

namespace shooter {
using namespace lam;

namespace {
constexpr float kSpreadAngle = 0.22f;                           // radians between spread shots
constexpr std::uint16_t kHurtInvulnTicks = 75;

// BurstAt needs an EmitterConfig id; tests run without particles, so the id may be none.
void burst(const Ctx &ctx, AssetId<EmitterConfig> config, Vec2 at, std::uint16_t count) {
    if (config) ctx.emit(BurstAt{config, at, count});
}

void spawn_bullet(Commands &cmd, Vec2 origin, Vec2 dir, float speed) {
    // Prefab = Transform + Velocity + Projectile + Lifetime + Bullet + Sprite/Animator; the
    // per-shot value (velocity) is recorded after the prefab's own components, so it wins.
    const Entity b = cmd.spawn_prefab(ids::kPrefabBullet, origin);
    cmd.add<Velocity>(b, Velocity{dir * speed});
}

void kill_enemy(const Ctx &ctx, Entity e, const Enemy &enemy, Vec2 at) {
    Arena &arena = ctx.local<Arena>();
    if (arena.phase != Phase::GameOver) {
        arena.score += enemy.points;
        ++arena.kills;
        ++arena.total_kills;
    }
    ctx.emit(EnemyKilled{at, enemy.kind, enemy.points});
    const auto count = static_cast<std::uint16_t>(enemy.kind == EnemyKind::Brute ? 32 : 12);
    if (const FxAssets *fx = ctx.try_res<FxAssets>()) burst(ctx, fx->death, at, count);
    ctx.emit(PlaySound{ids::kSndEnemyDie, at, true, 0.7f});
    ctx.commands().destroy(e);
    // Drops use their own RNG stream, so changing drop odds never shifts spawn rolls (rng.h).
    Rng &rng = ctx.rng(ids::kRngDrops);
    const float odds = enemy.kind == EnemyKind::Brute ? 0.6f : 0.04f;
    if (!rng.chance(odds)) return;
    const std::uint32_t roll = rng.below(10);
    const StrId prefab =
        roll < 3 ? ids::kPrefabHeart : (roll < 7 ? ids::kPrefabRapid : ids::kPrefabSpread);
    ctx.commands().spawn_prefab(prefab, at);
}
}

void restart_run(Ctx &ctx) {
    World &w = ctx.world();
    Commands &cmd = ctx.commands();
    for (auto [e, en] : w.view<Enemy>()) cmd.destroy(e);
    for (auto [e, b] : w.view<Bullet>()) cmd.destroy(e);
    for (auto [e, p] : w.view<Pickup>()) cmd.destroy(e);
    Arena &a = ctx.local<Arena>();
    a.phase = Phase::Intermission;
    a.phase_ticks = 90;
    a.wave = 0;
    a.to_spawn = 0;
    a.spawn_cursor = 0;
    a.score = 0;
    a.kills = 0;
    ++a.runs;
    for (auto [e, h, wpn, vel] : w.view<Health, Weapon, Velocity>(With<Player>{})) {
        h.hp = h.max_hp;
        h.invuln = 60;
        h.flash = 0;
        wpn.cooldown = wpn.rapid = wpn.spread = 0;
        wpn.trigger = false;
        vel.linear = {};
        cmd.teleport(e, player_start());                        // sets PrevTransform too: no glide
    }
}

void player_input(Ctx &ctx) {
    const InputFrame &in = input_frame(ctx);                    // latched tick input (or the bot's)
    for (const Intent &i : in.intent_list()) {
        if (i.kind == kIntentRestart) {                         // from the game-over menu
            restart_run(ctx);
            return;
        }
    }
    const Controls &c = ctx.res<Controls>();
    const bool alive = ctx.local<Arena>().phase != Phase::GameOver;
    World &w = ctx.world();
    auto players = w.view<Transform, Collider, TopDownMotor, Weapon>(With<Player>{});
    for (auto [e, t, col, motor, wpn] : players) {
        if (!alive) {
            motor.move = {};
            wpn.trigger = false;
            continue;
        }
        motor.move = clamp_length(in.axis2(c.move), 1.0f);
        // Twin-stick: a deflected aim stick (or arrow keys) aims and fires; otherwise the mouse
        // aims (pointer_world was latched with the focused world's camera) and the button fires.
        const Vec2 stick = in.axis2(c.aim);
        bool firing = in.is_held(c.fire);
        if (length_sq(stick) > 0.09f) {
            wpn.aim = normalize_or(stick, wpn.aim);
            firing = true;
        } else if (in.pointer_valid) {
            wpn.aim = normalize_or(in.pointer_world - (t.position + col.offset), wpn.aim);
        }
        wpn.trigger = firing;
    }
}

void tick_timers(Ctx &ctx) {
    World &w = ctx.world();
    for (auto [e, h] : w.view<Health>()) {
        if (h.invuln > 0) --h.invuln;
        if (h.flash > 0) --h.flash;
    }
    for (auto [e, wpn] : w.view<Weapon>()) {
        if (wpn.cooldown > 0) --wpn.cooldown;
        if (wpn.rapid > 0) --wpn.rapid;
        if (wpn.spread > 0) --wpn.spread;
    }
}

void fire_weapons(Ctx &ctx) {
    if (ctx.local<Arena>().phase == Phase::GameOver) return;
    World &w = ctx.world();
    Commands &cmd = ctx.commands();
    for (auto [e, t, col, wpn] : w.view<Transform, Collider, Weapon>(With<Player>{})) {
        if (!wpn.trigger || wpn.cooldown > 0) continue;
        wpn.cooldown =
            wpn.rapid > 0 ? static_cast<std::uint16_t>(wpn.fire_ticks / 2) : wpn.fire_ticks;
        const Vec2 origin = t.position + col.offset + wpn.aim * wpn.muzzle;
        spawn_bullet(cmd, origin, wpn.aim, wpn.bullet_speed);
        if (wpn.spread > 0) {
            spawn_bullet(cmd, origin, rotate(wpn.aim, kSpreadAngle), wpn.bullet_speed);
            spawn_bullet(cmd, origin, rotate(wpn.aim, -kSpreadAngle), wpn.bullet_speed);
        }
        ctx.emit(PlaySound{ids::kSndShoot, origin, false, 0.5f});
    }
}

void bullet_hits(Ctx &ctx) {
    World &w = ctx.world();
    const FxAssets *fx = ctx.try_res<FxAssets>();
    // ProjectileHit is emitted at Physics -1300 and read here in the same tick.
    for (const ProjectileHit &hit : ctx.read<ProjectileHit>()) {
        ctx.commands().destroy(hit.projectile);                 // pierce 0: one target per bullet
        if (fx) burst(ctx, fx->spark, hit.point, 4);
        if (hit.tile || !w.alive(hit.target)) continue;
        Health *h = w.try_get<Health>(hit.target);
        const Enemy *enemy = w.try_get<Enemy>(hit.target);
        if (h == nullptr || enemy == nullptr || h->hp <= 0) continue;   // already dead this tick
        const Bullet *b = w.try_get<Bullet>(hit.projectile);
        h->hp -= b ? b->damage : 1;
        h->flash = 5;
        if (h->hp > 0) {
            ctx.emit(PlaySound{ids::kSndHit, hit.point, true, 0.4f});
            continue;
        }
        kill_enemy(ctx, hit.target, *enemy, w.get<Transform>(hit.target).position);
    }
}

void contact_damage(Ctx &ctx) {
    Arena &arena = ctx.local<Arena>();
    if (arena.phase == Phase::GameOver) return;
    World &w = ctx.world();
    const FxAssets *fx = ctx.try_res<FxAssets>();
    for (auto [e, t, col, h] : w.view<Transform, Collider, Health>(With<Player>{})) {
        if (h.invuln > 0 || arena.god) continue;
        // Broadphase query (sorted by entity index: deterministic). 16 is plenty to pick damage.
        std::array<Entity, 16> touching{};
        const std::size_t n = overlap_box(w, collider_box(col, t.position), kLayerEnemy, touching);
        if (n == 0) continue;
        std::int32_t damage = 1;
        for (std::size_t i = 0; i < std::min(n, touching.size()); ++i)
            if (const Enemy *en = w.try_get<Enemy>(touching[i]))
                damage = std::max(damage, en->contact_damage);
        h.hp -= damage;
        h.invuln = kHurtInvulnTicks;
        h.flash = 12;
        const Vec2 at = t.position + col.offset;
        ctx.emit(PlayerHurt{at, h.hp});
        ctx.emit(PlaySound{ids::kSndHurt, at, false, 1.0f});
        if (fx) burst(ctx, fx->hurt, at, 10);
        if (h.hp > 0) continue;
        arena.phase = Phase::GameOver;
        ++arena.deaths;
        arena.best = std::max(arena.best, arena.score);
        ctx.emit(GameOver{arena.score, arena.wave});
        ctx.emit(PlaySound{ids::kSndGameOver, at, false, 1.0f});
        if (fx) burst(ctx, fx->death, at, 48);
    }
}

void collect_pickups(Ctx &ctx) {
    if (ctx.local<Arena>().phase == Phase::GameOver) return;
    World &w = ctx.world();
    const FxAssets *fx = ctx.try_res<FxAssets>();
    // Pickups are trigger colliders: physics reports TriggerEntered(trigger = pickup, other).
    for (const TriggerEntered &ev : ctx.read<TriggerEntered>()) {
        Pickup *p = w.try_get<Pickup>(ev.trigger);
        if (p == nullptr || !w.has<Player>(ev.other)) continue;
        Health *h = w.try_get<Health>(ev.other);
        Weapon *wpn = w.try_get<Weapon>(ev.other);
        switch (p->kind) {
        case PickupKind::Heart:
            if (h) h->hp = std::min(h->hp + 1, h->max_hp);
            break;
        case PickupKind::Rapid:
            if (wpn) wpn->rapid = p->power_ticks;
            break;
        case PickupKind::Spread:
            if (wpn) wpn->spread = p->power_ticks;
            break;
        }
        ctx.local<Arena>().score += 5;
        const Vec2 at = w.get<Transform>(ev.trigger).position;
        ctx.emit(PickupTaken{at, p->kind});
        ctx.emit(PlaySound{ids::kSndPickup, at, false, 0.8f});
        if (fx) burst(ctx, fx->pickup, at, 16);
        ctx.commands().destroy(ev.trigger);
    }
}
}
