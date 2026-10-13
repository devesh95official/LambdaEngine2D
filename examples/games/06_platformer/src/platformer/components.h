#pragma once
// Platformer data: tags, components, the Level resource and events. Rule state is reflected, so
// undo, saves and the state hash cover it. Global names, since prefabs refer to them by name.
#include <cstdint>
#include <lam/core/asset_id.h>
#include <lam/core/id.h>
#include <lam/ecs/component.h>
#include <lam/input/input_frame.h>
#include <lam/math/vec2.h>
#include <lam/particles/components.h>

// ---- Tags (filter with With<Tag>{}) ------------------------------------------------------------
struct Player {};
struct Coin {};
struct Goal {};

// ---- Components ----------------------------------------------------------------------------
// Enemy walker: drives its PlatformerMotor in `dir`, turning at walls and ledges.
struct Patrol { std::int8_t dir = -1; };
// Respawn point; `active` once touched (the animation switches to "on").
struct Checkpoint { bool active = false; };
// Per-hero rule state.
struct PlayerState {
    std::uint16_t invuln_ticks = 0;                             // > 0: hazards and enemies ignored
    bool was_grounded = false;                                  // last tick's motor.on_ground (jump sfx)
};

// ---- World resource (level world only, snapshotted) -------------------------------------------
struct Level {
    std::int32_t coins = 0, coins_total = 0;
    std::int32_t stomps = 0, deaths = 0, lives = 3;
    std::int32_t score = 0;
    lam::Vec2 respawn;                                          // feet position (checkpoint)
    float kill_y = 1e9f;                                        // feet below this = fell out
    std::uint64_t finish_tick = 0;                              // world tick of win / game over
    bool started = false;                                       // first tick with a hero seen
    bool complete = false;                                      // goal reached
    bool game_over = false;                                     // out of lives
};

// ---- App resources ---------------------------------------------------------------------------
struct Controls {                                               // action ids, resolved once
    lam::AxisAction move;                                       // x: run, y: climb (+y down)
    lam::ButtonAction jump;
};
// Particle configs for BurstAt, inserted by the executable. Absent in tests, so no BurstAt there.
struct FxIds {
    lam::AssetId<lam::EmitterConfig> coin, stomp, hurt, win;
};

// ---- Events (World scope) --------------------------------------------------------------------
// Emitted in PostPhysics when the level ends; the executable reads it at TickEnd and shows a scene.
struct LevelFinished { bool won = false; };

LAM_REFLECT_TAG(Player)
LAM_REFLECT_TAG(Coin)
LAM_REFLECT_TAG(Goal)
LAM_REFLECT(Patrol, dir)
LAM_REFLECT(Checkpoint, active)
LAM_REFLECT(PlayerState, invuln_ticks, was_grounded)
LAM_REFLECT_RESOURCE(Level, coins, coins_total, stomps, deaths, lives, score, respawn, kill_y, finish_tick, started,
            complete, game_over)
