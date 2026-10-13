#pragma once
// Lantern Run state: components, the per-world RunState and game events, all reflected or
// snapshotted. Transform.position is every actor's feet (bottom centre of the foot box).
#include <cstdint>
#include <lam/core/fsm.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/ecs/reflect_fsm.h>                                // Fsm<E> fields reflect with their owner
#include <lam/math/vec2.h>

namespace lantern {

enum class Facing : std::uint8_t { Down, Up, Left, Right };
enum class WolfState : std::uint8_t { Wander, Chase, Telegraph, Lunge, Recover };
enum class Phase : std::uint8_t { Title, Playing, Over };

// The traveller. Movement intent goes to its TopDownMotor; dash and knockback move it directly.
struct Player {
    std::int32_t hearts = 3;
    std::int32_t protection = 0;                                // ticks of after-hit protection
    std::int32_t dash_left = 0;
    std::int32_t dash_cooldown = 0;
    lam::Vec2 dash_dir;
    lam::Vec2 knockback;                                        // px/s, decays by knockback_friction
    Facing facing = Facing::Down;
    bool alive = true;
    bool dashing() const { return dash_left > 0; }
    bool vulnerable() const { return alive && protection == 0 && dash_left == 0; }
};

// One wolf. `fsm` times the fixed-length states (Telegraph, Lunge, Recover use
// fsm.ticks_in_state); `timer` is for Wander's random pauses.
struct Wolf {
    lam::Fsm<WolfState> fsm;
    lam::Vec2 home;                                             // den feet
    lam::Vec2 goal;                                             // wander goal
    lam::Vec2 dir{1.0f, 0.0f};                                  // lunge direction
    Facing facing = Facing::Down;
    std::int32_t timer = 0;
    std::int32_t cooldown = 0;
    std::int32_t unseen = 0;
    std::uint32_t slot = 0;                                     // spawn order
    bool hit_this_lunge = false;
    bool lunging = false;                                       // moved by a lunge this tick
    bool wander_step = false;                                   // asked to walk to `goal` this tick
    bool moving = false;                                        // moved > 0.05 px last tick (animation)
};

// A lantern: re-placed (teleported) when collected; `present` false when no spot was found.
struct Lantern {
    bool present = false;
};

// Scenery tags (static; their solidity lives in the TileWorld).
struct Tree {};
struct Rock {};

// Per-world run state (World resource, snapshotted).
struct RunState {
    Phase phase = Phase::Title;
    std::int32_t collected = 0;
    std::int32_t wave = 1;
    std::int32_t best = 0;
    std::int32_t runs = 0;                                      // runs started
    std::int32_t deaths = 0;
    std::uint64_t over_at = 0;                                  // world tick of the last death
    std::uint32_t wolves_spawned = 0;                           // this run: den rotation and slots
};

// Events (World scope). The rules emit them in tick stages; the executable turns them into
// particles (BurstAt), camera shake and HUD banners, and tests count them.
struct LanternCollected { lam::Vec2 at; std::int32_t total = 0; };
struct PlayerDashed { lam::Vec2 at; };
struct PlayerHurt { lam::Vec2 at; std::int32_t hearts_left = 0; };
struct PlayerDied { lam::Vec2 at; std::int32_t collected = 0; };
struct WaveUp { std::int32_t wave = 0; };
struct WolfWindup { lam::Entity wolf; };

} // namespace lantern

LAM_REFLECT_ENUM(lantern::Facing, "down", "up", "left", "right")
LAM_REFLECT_ENUM(lantern::WolfState, "wander", "chase", "telegraph", "lunge", "recover")
LAM_REFLECT_ENUM(lantern::Phase, "title", "playing", "over")
LAM_REFLECT(lantern::Player, hearts, protection, dash_left, dash_cooldown, dash_dir, knockback, facing, alive)
LAM_REFLECT(lantern::Wolf, fsm, home, goal, dir, facing, timer, cooldown, unseen, slot, hit_this_lunge, lunging,
            wander_step, moving)
LAM_REFLECT(lantern::Lantern, present)
LAM_REFLECT_TAG(lantern::Tree)
LAM_REFLECT_TAG(lantern::Rock)
LAM_REFLECT(lantern::RunState, phase, collected, wave, best, runs, deaths, over_at, wolves_spawned)
