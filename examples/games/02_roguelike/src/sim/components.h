#pragma once
// Roguelike state: components, World resources and events, all reflected for prefabs, saves, undo
// and the state hash. The rules also use the engine's TurnActor/PlayerTurn + TurnScheduler,
// GridPosition/GridMover, TilemapInstance (the carved dungeon) and FovMemory.
#include <array>
#include <cstdint>
#include <lam/core/fixed_string.h>
#include <lam/core/id.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/vec2.h>

namespace rl {
using namespace lam::literals;

// ---- Components ------------------------------------------------------------------------------
struct Player {};     // tag: the hero (also has lam::PlayerTurn)
struct FloorLocal {}; // tag: destroyed when the floor changes
struct Monster {
    lam::FixedString<16> name; // for messages ("the goblin hits you")
    std::uint8_t sight = 8;    // cells; wakes when it sees the player
    bool awake = false;        // chases the player once awake
};
struct Stats {
    std::int16_t hp = 10, max_hp = 10;
    std::int16_t attack = 2, defense = 0;
    std::uint16_t xp = 0;   // monsters: xp granted when killed
    std::uint8_t level = 1; // player only
};
// On the floor; def = GameData::items id (saved by name, so saves survive data edits).
struct Item {
    lam::StrId def;
};
inline constexpr int kInventorySlots = 8;
struct Inventory { // player's pack: item ids, packed from 0
    std::array<lam::StrId, kInventorySlots> slots{};
    std::uint8_t count = 0;
};

// ---- World resources -------------------------------------------------------------------------
enum class Phase : std::uint8_t { NeedFloor, Playing, Dead };
// The run: snapshotted, saved and hashed (setup_run registers it with snapshot_resource).
struct RunState {
    std::uint64_t run_seed = 1; // floor N is generated from derive_seed(run_seed, N)
    Phase phase = Phase::NeedFloor;
    std::uint16_t depth = 1;
    lam::Entity map; // the Tilemap entity carved by generate_floor
    lam::Entity player;
    lam::IVec2 stairs; // cell of the down stairs on this floor
    std::uint32_t kills = 0;
    std::uint32_t score = 0;
    std::uint32_t floor_turns = 0; // player turns spent on this floor
    std::uint32_t turns = 0; // player turns this run (TurnScheduler::turns counts every actor)
    // A kUseItem intent that arrived while it was not the player's turn (steps still animating):
    // intents live for one tick only, so the rules keep it here (saved, hashed) until the turn.
    std::int8_t pending_use = -1;
};

// ---- Events ----------------------------------------------------------------------------------
// Emitted on every attack; presentation turns it into sparks and shake.
struct Hit {
    lam::Entity attacker, target;
    lam::IVec2 cell;
    std::int16_t damage = 0;
    bool killed = false;
};
} // namespace rl

LAM_REFLECT_TAG(rl::Player)
LAM_REFLECT_TAG(rl::FloorLocal)
LAM_REFLECT(rl::Monster, name, sight, awake)
LAM_REFLECT(rl::Stats, hp, max_hp, attack, defense, xp, level)
LAM_REFLECT(rl::Item, def)
LAM_REFLECT(rl::Inventory, slots, count)
LAM_REFLECT_ENUM(rl::Phase, "need_floor", "playing", "dead")
LAM_REFLECT(rl::RunState, run_seed, phase, depth, map, player, stairs, kills, score, floor_turns,
            turns, pending_use)
