#pragma once
// Shooter components, game events and collision layers. Plain reflected data: prefabs name them,
// and saves, undo and the state hash cover them.
#include <cstdint>
#include <lam/core/asset_id.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/vec2.h>
#include <lam/particles/components.h>
namespace shooter {

// Collision layers (Collider::layer / mask bits). Tiles block motors via solid_mask = kLayerWall.
inline constexpr std::uint32_t kLayerWall = 1;
inline constexpr std::uint32_t kLayerPlayer = 2;
inline constexpr std::uint32_t kLayerEnemy = 4;
inline constexpr std::uint32_t kLayerPickup = 8;

// Tag on the player entity.
struct Player {};

// Hit points. `invuln`: ticks of immunity after a hit; `flash`: ticks of hit flash (the
// executable tints the sprite; counted in ticks so it replays).
struct Health {
    std::int32_t hp = 3;
    std::int32_t max_hp = 3;
    std::uint16_t invuln = 0;
    std::uint16_t flash = 0;
};

enum class EnemyKind : std::uint8_t { Grunt, Runner, Brute };

// Every chaser. Its speed lives in its TopDownMotor (prefab data), not here.
struct Enemy {
    EnemyKind kind = EnemyKind::Grunt;
    std::int32_t points = 10;
    std::int32_t contact_damage = 1;
    float radius = 6;                                           // separation radius (px)
};

// The player's gun. `aim` and `trigger` are intent (written in Stage::Input from the InputFrame);
// cooldown and power-up timers are rule state counted in ticks.
struct Weapon {
    lam::Vec2 aim{1, 0};                                        // unit vector
    bool trigger = false;
    std::uint16_t cooldown = 0;                                 // ticks until the next shot
    std::uint16_t fire_ticks = 9;                               // base ticks between shots
    std::uint16_t rapid = 0;                                    // ticks of rapid fire left
    std::uint16_t spread = 0;                                   // ticks of 3-way shot left
    float bullet_speed = 330;                                   // px/s
    float muzzle = 8;                                           // spawn offset from body centre
};

// A bullet's payload. The engine's Projectile (swept, Physics -1300) finds the target and emits
// ProjectileHit; bullet_hits (PostPhysics) applies `damage`.
struct Bullet {
    std::int32_t damage = 1;
};

enum class PickupKind : std::uint8_t { Heart, Rapid, Spread };

// A trigger collider the player walks over (TriggerEntered, PostPhysics).
struct Pickup {
    PickupKind kind = PickupKind::Heart;
    std::uint16_t power_ticks = 600;                            // rapid/spread duration
};

// ---- Game events (World scope; emitted by tick systems, read by the executable in Update) ------
struct EnemyKilled {
    lam::Vec2 position;
    EnemyKind kind = EnemyKind::Grunt;
    std::int32_t points = 0;
};
struct PlayerHurt { lam::Vec2 position; std::int32_t hp_left = 0; };
struct WaveStarted { std::uint32_t wave = 0; std::uint32_t enemies = 0; };
struct WaveCleared { std::uint32_t wave = 0; std::int64_t bonus = 0; };
struct GameOver { std::int64_t score = 0; std::uint32_t wave = 0; };
struct PickupTaken { lam::Vec2 position; PickupKind kind = PickupKind::Heart; };

// Particle configs for BurstAt. An App resource the executable fills at startup
// (AssetServer::add<EmitterConfig>); tests may leave the ids empty (no particles drawn).
struct FxAssets {
    lam::AssetId<lam::EmitterConfig> death, spark, pickup, hurt;
};
}
LAM_REFLECT_TAG(shooter::Player)
LAM_REFLECT(shooter::Health, hp, max_hp, invuln, flash)
LAM_REFLECT_ENUM(shooter::EnemyKind, "grunt", "runner", "brute")
LAM_REFLECT(shooter::Enemy, kind, points, contact_damage, radius)
LAM_REFLECT(shooter::Weapon, aim, trigger, cooldown, fire_ticks, rapid, spread, bullet_speed,
            muzzle)
LAM_REFLECT(shooter::Bullet, damage)
LAM_REFLECT_ENUM(shooter::PickupKind, "heart", "rapid", "spread")
LAM_REFLECT(shooter::Pickup, kind, power_ticks)
