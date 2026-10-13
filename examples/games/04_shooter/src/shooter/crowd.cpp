// Crowd steering: flow field toward the player + separation.
#include <shooter/crowd.h>

#include <array>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/components.h>
#include <lam/physics/motors.h>
#include <lam/physics/queries.h>
#include <lam/physics/tile_world.h>
#include <shooter/arena.h>
#include <shooter/components.h>

namespace shooter {
using namespace lam;

namespace {
constexpr float kSeekRadius = 40;                               // closer: steer straight at it
constexpr float kSeparationWeight = 0.9f;
}

void enemy_steering(Ctx &ctx) {
    World &w = ctx.world();
    Crowd &crowd = ctx.local<Crowd>();
    const TileWorld &tiles = ctx.local<TileWorld>();
    const Entity player = find_player(w);
    if (!player) return;
    const Transform &pt = w.get<Transform>(player);
    const Collider *pc = w.try_get<Collider>(player);
    const Vec2 target = pt.position + (pc ? pc->offset : Vec2{});

    // One Dijkstra pass per player cell change serves every enemy (hundreds share it).
    const IVec2 cell = tiles.tile_at(target);
    if (!crowd.valid || cell != crowd.goal) {
        const std::array<IVec2, 1> goals{cell};
        crowd.distance.compute(tiles.cost, goals, GridDirs::Eight);
        crowd.flow.compute(crowd.distance);
        crowd.goal = cell;
        crowd.valid = true;
    }

    // Only motor intent is written here. Steer from the body centre: a flow direction taken at the
    // feet would aim the body's top edge into the corners of pillars it skirts.
    for (auto [e, t, col, motor, enemy] : w.view<Transform, Collider, TopDownMotor, Enemy>()) {
        const Vec2 body = t.position + col.offset;
        const Vec2 to_player = target - body;
        const bool close = length_sq(to_player) < kSeekRadius * kSeekRadius;
        Vec2 dir = close ? normalize_or(to_player) : crowd.flow.direction(body, tiles);
        if (dir == Vec2{}) dir = normalize_or(to_player);       // goal/unreachable: go straight
        const Vec2 push = separation(w, e, enemy.radius * 2.0f, 1.0f);
        motor.move = clamp_length(dir + push * kSeparationWeight, 1.0f);
    }
}
}
