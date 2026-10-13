#pragma once
// Public entry points of the roguelike rules (roguelike_sim links only pure engine targets).
//   app.add_plugin<rl::RulesPlugin>();   // components, events, tick systems
//   rl::load_game_data(app);             // prefabs + item/monster catalogs
//   rl::setup_run(app, world, seed);     // in Scene::enter, or on app.main_world() in tests
// The rules then run by themselves on every world holding a RunState: generate_floor and
// take_turns in Simulate, driven by the InputFrame (keyboard, UI intents, or rl::BotDriver).
#include <cstdint>
#include <lam/core/app.h>
#include <sim/components.h>
#include <string_view>

namespace rl {
// Components, events and the tick systems, gated on has_local<RunState>:
//   Simulate  +0  generate_floor   (waits for the Tilemap instance; carves floor N)
//   Simulate +10  take_turns       (next_actor/end_turn loop: player input, monster AI)
// Requires lam.tilemap (GridIndex, TilemapInstance) and lam.input (InputFrame).
class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "game.roguelike.rules"; }
    void build(lam::App &app) override;
};

// Prepares `world` for a new run: the World resources (RunState, TurnScheduler, FovMemory,
// FogStyle, MessageLog, the AI cache), the Tilemap entity (maps/floor.tmj, an all-wall template)
// and the player. generate_floor carves floor 1 once the map is Ready. Needs load_game_data first.
void setup_run(lam::App &app, lam::World &world, std::uint64_t run_seed);

// Read-only helpers for presentation, the bot and tests.
const RunState *run_state(const lam::World &w);          // null if the world holds no run
bool cell_visible(const lam::World &w, lam::IVec2 cell); // in the player's FOV this turn
bool cell_explored(const lam::World &w, lam::IVec2 cell);
std::uint64_t score_of(const lam::World &w);
} // namespace rl
