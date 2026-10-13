#pragma once
// Energy-based turn scheduler for roguelikes. Highest energy acts first, ties by entity index.
//   for (Entity e; (e = next_actor(w)) && !w.has<PlayerTurn>(e);) { act(w, e); end_turn(w, e); }
#include <cstdint>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
namespace lam {
class World;
// Energy needed to act; also the default cost of a turn.
inline constexpr std::int32_t kTurnThreshold = 1000;
// Gains `speed` energy per scheduler round.
struct TurnActor { std::int32_t energy = 0; std::int32_t speed = 100; };
struct PlayerTurn {};                                           // tag: the scheduler waits for input
// World resource; snapshotted, hashed and saved.
struct TurnScheduler {
    Entity current;                                             // actor whose turn it is (until end_turn)
    bool busy = false;                                          // animations playing (grid movers set it)
    std::uint16_t max_actors_per_tick = 64;
    std::uint16_t acted_this_tick = 0;
    std::uint64_t last_tick = 0;
    std::uint64_t turns = 0;                                    // completed turns
};
// Inserts the scheduler if absent and registers it for snapshots.
TurnScheduler &add_turn_scheduler(World &w);
// `current`, or else grants energy until someone can act. Null if busy, out of per-tick budget,
// or nobody can ever act.
Entity next_actor(World &w);
// What next_actor() would return, without changing any state. Safe from input, bots and UI.
Entity peek_actor(const World &w);
void end_turn(World &w, Entity actor, std::int32_t cost = kTurnThreshold);
// next_actor() returns null while busy, e.g. during move animations.
void set_turns_busy(World &w, bool busy);
}
LAM_REFLECT(lam::TurnActor, energy, speed)
LAM_REFLECT_TAG(lam::PlayerTurn)
LAM_REFLECT(lam::TurnScheduler, current, busy, max_actors_per_tick, acted_this_tick, last_tick, turns)
