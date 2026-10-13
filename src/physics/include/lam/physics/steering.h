#pragma once
// Steering helpers. Tiles only, no entities; velocities are px/s.
// `box` is relative to the position arguments: collider_box(collider, {0, 0}).
#include <lam/math/rect.h>
#include <lam/math/rng.h>
#include <lam/math/vec2.h>
#include <lam/physics/tile_world.h>
namespace lam {
Vec2 seek(Vec2 from, Vec2 to, float speed);
// If the way is blocked within 24 px, tries +-45, +-90, +-135 degrees in that order.
Vec2 flee(const TileWorld &w, Rect box, Vec2 pos, Vec2 threat, float speed);
// Circles a target at keep_distance: approaches when too far, backs off under 0.75x, otherwise
// orbits (orbit_sign +1 = clockwise). keep_distance <= 0 is plain seek().
Vec2 hold_ring(Vec2 pos, Vec2 target, float keep_distance, float approach_speed, float orbit_speed,
               int orbit_sign = 1);
// Tries `attempts` random points around `home` and takes the first one reachable in a straight
// line. Leaves `out` alone and returns false if none is.
bool pick_wander_goal(const TileWorld &w, Rect box, Vec2 home, float radius, Rng &rng, Vec2 &out,
                      int attempts = 8);
// Old e2d variant: checks the path from `from` instead of `home`.
bool pick_wander_goal(const TileWorld &w, Rect box, Vec2 from, Vec2 home, float radius, Rng &rng,
                      Vec2 &out, int attempts = 4);
// Exact sweep against blocking tiles. Grazing a tile corner counts as blocked.
bool box_path_clear(const TileWorld &w, Rect box, Vec2 a, Vec2 b);
// Within range and no kTileOpaque tile in between. Use sight_point() for bodies.
bool can_see(const TileWorld &w, Vec2 eye, Vec2 target, float range);
// 1 px above the bottom centre of an absolute body box, so it's inside the body's own tile.
Vec2 sight_point(const Rect &body);
}
