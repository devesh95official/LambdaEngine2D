#pragma once
// Every tuning number of Lantern Run.
// lantern::Tuning is an App resource (LanternRunSimPlugin inserts the defaults), so tests and
// mods can change it before the first tick; the rules only read it. Speeds are px/s, durations
// are ticks at 60 Hz, distances are pixels.
#include <cstdint>
#include <lam/math/vec2.h>

namespace lantern {

struct Tuning {
    // Walking -> TopDownMotor fields.
    float walk_speed = 100.0f;
    float walk_accel = 1000.0f;
    float walk_decel = 1300.0f;
    float walk_min_speed = 8.0f;                                // below it the walk clip stops
    float corner_assist = 5.0f;
    // Foot boxes (width x height, anchored at the feet) -> Collider half/offset.
    lam::Vec2 player_box{10.0f, 6.0f};
    lam::Vec2 wolf_box{12.0f, 6.0f};

    int hearts = 3;
    float dash_speed = 240.0f;
    int dash_ticks = 14;
    int dash_cooldown = 50;
    int hurt_protection = 90;
    float knockback_speed = 180.0f;
    float knockback_friction = 900.0f;

    float wolf_sight = 170.0f;
    float wolf_wander_speed = 35.0f;
    float wolf_chase_speed = 72.0f;
    float wolf_speed_per_wave = 0.06f;                          // +6% per wave
    float wolf_lunge_range = 36.0f;
    int wolf_telegraph = 26;
    int wolf_aim_lock = 8;                                      // the last ticks of the wind-up keep their aim
    int wolf_lunge = 12;
    float wolf_lunge_speed = 230.0f;
    int wolf_recover = 36;
    int wolf_cooldown = 40;
    int wolf_lose_ticks = 150;
    float wolf_separation_radius = 18.0f;
    float wolf_separation_strength = 40.0f;
    float wolf_ring = 48.0f;                                    // circling distance while on cooldown
    float wolf_wander_radius = 64.0f;
    int wolf_repath_ticks = 30;
    int max_wolves = 8;
    int start_wolves = 3;

    int lanterns_at_once = 4;
    int lanterns_per_wave = 8;
    float pickup_radius = 14.0f;
    float lantern_min_distance = 100.0f;                        // from the player when one appears
    float lantern_spacing = 48.0f;                              // between lit lanterns

    int path_searches_per_tick = 2;                             // A* searches per tick, whole pack
    int restart_after_ticks = 120;                              // the bot starts a new run this long after dying
};

} // namespace lantern
