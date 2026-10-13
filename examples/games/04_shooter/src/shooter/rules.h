#pragma once
// The shooter's tick systems (plain functions; all state in components and the Arena resource).
// Stage map (game orders are inside (-1000, 1000), so they run between the engine's systems):
//   Input        0  player_input        InputFrame -> motor move, weapon aim/trigger, restart
//   Simulate     0  tick_timers         cooldowns, invulnerability, power-ups (ticks)
//   Simulate    10  wave_director       intermission -> wave -> spawn budget per tick
//   Simulate    30  fire_weapons        Commands::spawn_prefab("bullet"), PlaySound
//   PostPhysics  0  bullet_hits         ProjectileHit -> damage, kills, drops, BurstAt
//   PostPhysics 10  contact_damage      overlap_box(player, enemies) -> PlayerHurt / GameOver
//   PostPhysics 20  collect_pickups     TriggerEntered(pickup, player)
//   PostPhysics 30  enemy_steering      (crowd.h) flow field + separation -> TopDownMotor::move
// Spatial queries (overlap_box, separation) run in PostPhysics, after this tick's movement.
// Effects go out as PlaySound / BurstAt events.
#include <lam/core/app.h>
namespace shooter {
void player_input(lam::Ctx &ctx);
void tick_timers(lam::Ctx &ctx);
void wave_director(lam::Ctx &ctx);
void fire_weapons(lam::Ctx &ctx);
void bullet_hits(lam::Ctx &ctx);
void contact_damage(lam::Ctx &ctx);
void collect_pickups(lam::Ctx &ctx);
// Applies the restart intent: destroys enemies, bullets and pickups (Commands), resets the
// player and the run counters, keeps the session counters. Called by player_input.
void restart_run(lam::Ctx &ctx);
}
