#pragma once
// String ids used by the rules, hashed at compile time. Names match prefabs.json and the sound
// bank groups.
#include <lam/core/id.h>
namespace shooter::ids {
// Prefabs (assets/prefabs.json).
inline constexpr lam::StrId kPrefabPlayer = lam::str_id("player");
inline constexpr lam::StrId kPrefabBullet = lam::str_id("bullet");
inline constexpr lam::StrId kPrefabGrunt = lam::str_id("grunt");
inline constexpr lam::StrId kPrefabRunner = lam::str_id("runner");
inline constexpr lam::StrId kPrefabBrute = lam::str_id("brute");
inline constexpr lam::StrId kPrefabHeart = lam::str_id("pickup_heart");
inline constexpr lam::StrId kPrefabRapid = lam::str_id("pickup_rapid");
inline constexpr lam::StrId kPrefabSpread = lam::str_id("pickup_spread");
// Sound bank groups (PlaySound::group; the executable builds the bank).
inline constexpr lam::StrId kSndShoot = lam::str_id("shoot");
inline constexpr lam::StrId kSndHit = lam::str_id("hit");
inline constexpr lam::StrId kSndEnemyDie = lam::str_id("enemy_die");
inline constexpr lam::StrId kSndHurt = lam::str_id("hurt");
inline constexpr lam::StrId kSndPickup = lam::str_id("pickup");
inline constexpr lam::StrId kSndWave = lam::str_id("wave");
inline constexpr lam::StrId kSndGameOver = lam::str_id("game_over");
// Per-world RNG streams (World::rng): one per independent rule, so they never shift each other.
inline constexpr lam::StrId kRngSpawn = lam::str_id("shooter.spawn");
inline constexpr lam::StrId kRngDrops = lam::str_id("shooter.drops");
// Menu protocol of the engine menu scenes (lam/ui/menu_scenes.h): a = item index.
inline constexpr lam::StrId kIntentMenuSelect = lam::str_id("menu.select");
}
