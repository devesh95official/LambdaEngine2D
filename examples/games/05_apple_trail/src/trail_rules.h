#pragma once
// Apple Trail rules (apple_trail_sim, no raylib). The traveller walks the orchard a tile at a time,
// each apple adds a piece to the trail behind, and hitting a wall or the trail ends the run.
//  - Pieces are entities with GridPosition + GridMover; the rules move them a cell per step and
//    the engine's grid_movers (Animate -1200) tweens Transform.
//  - Self-collision asks GridIndex::at(cell) (rebuilt at Simulate -1500, before trail.step).
//  - All run state is one snapshotted World resource (TrailState).
//  - Effects go out as BurstAt and ShakeAt events.
//  - Systems are gated with run_if = has_local<TrailState>, so title and menu worlds skip them.
#include <string_view>
#include <lam/core/app.h>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
#include <lam/math/rng.h>
#include "trail_components.h"
#include "trail_map.h"

namespace trail {

// Action ids (App resource), resolved once from ActionMap at startup. The bot presses these.
struct Controls {
    lam::ButtonAction up, down, left, right;
    lam::ButtonAction confirm;                                  // start, play again
    lam::ButtonAction pause;
};

lam::IVec2 step(lam::IVec2 from, Dir dir);
bool opposite(Dir a, Dir b);
// Ticks per tile after `eaten` apples (the pace quickens every kRules.apples_per_speedup).
int step_ticks_for(int eaten);
// A turn press. Up to two are queued, so a quick "up, left" both happen on the next tiles.
// A repeat of the last queued heading and a reversal into the trail are ignored.
void queue_turn(TrailState &s, Dir dir);

// Starts a run on `w`: fills the world's TileWorld from the orchard, inserts TrailState
// (snapshot_resource on first use), spawns the traveller, the starting trail and the first apple.
// Call from Scene::enter or a startup system (World::rng is used, so it is deterministic).
void start_run(lam::World &w, const Orchard &orchard);
// The first cell that is grass and holds no piece or apple, tried at random first (stream
// "trail.apples"), then in reading order. Cold path (a plain view over GridPosition).
lam::IVec2 random_free_cell(lam::World &w, lam::Rng &rng);

// Systems (registered by TrailRulesPlugin; exposed for tests and --print-schedule readers).
void steer_system(lam::Ctx &ctx);                               // Input: presses -> turn buffer
void step_system(lam::Ctx &ctx);                                // Simulate: rot, step, eat, crash

class TrailRulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "game.apple_trail.rules"; }
    // Components, events, and the two systems above (run_if has_local<TrailState>).
    void build(lam::App &app) override;
};

} // namespace trail
