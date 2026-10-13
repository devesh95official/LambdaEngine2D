#pragma once
// Crowd steering: one Dijkstra map + FlowField toward the player's cell, shared by every enemy,
// plus `separation` so they don't stack. Crowd isn't snapshotted: the field only depends on the
// goal cell and the static map, so it is rebuilt when the goal changes and survives undo/load.
#include <lam/core/app.h>
#include <lam/math/vec2.h>
#include <lam/tilemap/grid_maps.h>
namespace shooter {

// World resource (inserted by setup_arena). Not in snapshots: recomputed from state.
struct Crowd {
    lam::DijkstraMap distance;
    lam::FlowField flow;
    lam::IVec2 goal{-1, -1};                                    // cell the field points to
    bool valid = false;
};

// PostPhysics +30 (after this tick's broadphase, so separation() sees fresh positions):
// refresh the field if the player changed cell, then write every enemy's
// TopDownMotor::move = flow direction (direct seek when close) + separation.
void enemy_steering(lam::Ctx &ctx);
}
