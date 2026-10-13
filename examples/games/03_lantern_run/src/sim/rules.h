#pragma once
// Internal to lantern_run_sim: the rule systems and the helpers they share. Each system is a
// plain function registered by LanternRunSimPlugin at the stage noted beside it.
// Tick order (one fixed step):
//   Simulate      0  player_rules     dash, knockback (move_body), walk intent (TopDownMotor)
//   Simulate     10  wolf_brains      Fsm<WolfState>, sight, steering, PathRequest/follow_route
//   Simulate    100  run_phase        title/over -> playing (confirm), new runs
//   Physics   (engine) motors move the player and the wolves against the TileWorld
//   PostPhysics   0  lantern_pickups  collect, re-place, waves (+1 wolf)
//   PostPhysics  10  wolf_bites       lunge bites (last one: Over + stop_actors), stuck wanderers, `moving`
//   PostPhysics  20  choose_clips     4-direction Animator clips from sim state (shown this tick)
//   Animate       0  face_sprites     Sprite kFlipX for left-facing actors (after the animators)
#include <lam/core/app.h>
#include <lam/ecs/entity.h>
#include <lam/math/rect.h>
#include <lam/math/rng.h>
#include <lam/math/vec2.h>

#include "sim/components.h"
#include "sim/tuning.h"

namespace lam {
class World;
struct TileWorld;
}

namespace lantern {

// Collision layers: bodies are never solid to each other (every body moves against the tile grid
// only), so they sit on their own layers and the motors' solid_mask is kSolidLayer.
inline constexpr std::uint32_t kSolidLayer = 1;
inline constexpr std::uint32_t kPlayerLayer = 2;
inline constexpr std::uint32_t kWolfLayer = 4;

const Tuning &tuning(const lam::Ctx &ctx);
lam::Rng &rules_rng(const lam::Ctx &ctx);                       // one stream for all the rules

// A foot box relative to the feet: [-w/2, w/2) x [-h, 0). steering.h takes boxes in this form.
lam::Rect foot_box(lam::Vec2 size);
lam::Rect body_at(lam::Vec2 feet, lam::Vec2 size);              // the same box in world pixels
lam::Vec2 facing_vector(Facing f);
Facing facing_for(lam::Vec2 dir, Facing current);               // larger axis wins; ties keep current
lam::Vec2 clamp_input(lam::Vec2 v);                             // unit length at most; NaN -> 0
lam::IVec2 standing_tile(const lam::TileWorld &tiles, lam::Vec2 feet, lam::Vec2 box);

// Runs (run.cpp)
void start_run(lam::Ctx &ctx);                                  // fresh player, wolves, lanterns
lam::Entity spawn_wolf(lam::Ctx &ctx);                          // null at max_wolves
void place_lantern(lam::Ctx &ctx, lam::Entity lantern);
// Zeroes every motor's intent and velocity. The engine's motors run every tick whatever the
// game's run_if says, so a run that ends must stop its bodies.
void stop_actors(lam::World &w);

// Systems
bool is_playing(const lam::Ctx &ctx);                           // run_if
void setup_forest(lam::Ctx &ctx);                               // startup
void run_phase(lam::Ctx &ctx);
void player_rules(lam::Ctx &ctx);
void wolf_brains(lam::Ctx &ctx);
void lantern_pickups(lam::Ctx &ctx);
void wolf_bites(lam::Ctx &ctx);
void choose_clips(lam::Ctx &ctx);
void face_sprites(lam::Ctx &ctx);

} // namespace lantern
