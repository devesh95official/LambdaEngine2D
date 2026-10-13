#pragma once
// Internal to roguelike_sim: shared helpers of the rule files (dungeon, turns, combat, items).
// Every function here runs inside a tick stage (Simulate) on the world of `ctx`.
#include <cstdint>
#include <lam/core/app.h>
#include <lam/core/fixed_string.h>
#include <lam/math/color.h>
#include <lam/math/grid.h>
#include <lam/tilemap/grid_maps.h>
#include <lam/tilemap/tileset.h>
#include <sim/components.h>
#include <string_view>

namespace rl {
// Map layout constants shared with assets/maps/floor.tmj and assets/maps/dungeon.tsj.
inline constexpr lam::StrId kLayer = lam::str_id("dungeon"); // the tile layer the rules carve
inline constexpr lam::TileId kWallGid = 1, kFloorGid = 2, kFloorAltGid = 3, kStairsGid = 4,
                             kWallAltGid = 5;
inline constexpr int kFovRadius = 8;
inline constexpr std::int32_t kTurnCost = 1000; // == lam::kTurnThreshold: one action

// World resource, not reflected: the Dijkstra map monsters walk down toward the player. Derived
// from the TileWorld and the player's cell; a restore hook and the tile version invalidate it.
struct ChaseMap {
    lam::DijkstraMap map;
    lam::Grid<std::uint16_t> cost; // 0 = wall (impassable), 10 = floor
    lam::IVec2 goal{-1, -1};
    std::uint32_t tile_version = 0;
    bool valid = false;
};

// Message log line (World resource lam::MessageLog). Allocation-free formatting.
void say(lam::World &w, std::string_view text, lam::Color c = lam::kWhite);

// Vision: recomputes FovMemory around the player; wakes monsters that can see the player.
void refresh_fov(lam::World &w);

// Floor generation (dungeon.cpp). Returns false while the map instance is not ready yet.
bool generate_floor(const lam::Ctx &ctx);

// Combat (combat.cpp): resolves one melee attack; kills, xp, level-ups, death and events.
void attack(const lam::Ctx &ctx, lam::Entity attacker, lam::Entity target);
void damage(const lam::Ctx &ctx, lam::Entity attacker, lam::Entity target, int amount);

// Items (items.cpp). Returns the energy cost (0 = nothing happened, the turn is not spent).
std::int32_t use_item(const lam::Ctx &ctx, lam::Entity player, int slot);
void pick_up_items(const lam::Ctx &ctx, lam::Entity player, lam::IVec2 cell);

// Turns (turns.cpp): the Simulate system.
void take_turns(lam::Ctx &ctx);
void generate_floor_system(lam::Ctx &ctx);

// The chase map, refreshed when the player moved or the tiles changed.
const ChaseMap &chase_map(lam::World &w, lam::IVec2 player_cell);
} // namespace rl
