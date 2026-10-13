// Wolves: a five-state machine (lam::Fsm<WolfState> in the Wolf component), split over stages:
//  * wolf_brains (Simulate) decides and writes TopDownMotor intent; the motor moves the wolf in
//    Physics; wolf_bites (PostPhysics) checks the lunge bite and the "stuck" wanderer after the
//    move.
//  * A* goes through PathRequest -> serve_path_requests (Simulate -1100, next tick) -> PathRoute,
//    whose computed_tick drives route_stale.
#include <algorithm>
#include <array>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/geometry.h>
#include <lam/physics/motors.h>
#include <lam/physics/queries.h>
#include <lam/physics/steering.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/grid_path.h>
#include <lam/tilemap/path_follow.h>

#include "sim/rules.h"

namespace lantern {

namespace {

constexpr int kMaxPack = 32;

// Velocity -> motor intent. max_speed is the cap for this tick, so |move| <= 1 always holds.
void drive(lam::TopDownMotor &motor, lam::Vec2 velocity, float cap) {
    motor.max_speed = cap;
    motor.move = cap > 0.0f ? lam::clamp_length(velocity / cap, 1.0f) : lam::Vec2{};
}

} // namespace

void wolf_brains(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const Tuning &t = tuning(ctx);
    const lam::TileWorld *tiles = ctx.try_local<lam::TileWorld>();
    const RunState &run = ctx.local<RunState>();
    lam::Entity pe;
    const Player *player = w.single_or_null<Player>(&pe);
    if (tiles == nullptr || player == nullptr) return;
    const lam::Vec2 target = w.get<lam::Transform>(pe).position;
    const lam::Vec2 target_eye = lam::sight_point(body_at(target, t.player_box));
    const float speed = t.wolf_chase_speed * (1.0f + t.wolf_speed_per_wave * static_cast<float>(run.wave - 1));
    const std::uint64_t now = w.tick();
    lam::Rng &rng = rules_rng(ctx);

    // Positions at the start of the tick, by spawn slot.
    std::array<lam::Vec2, kMaxPack> positions{};
    std::size_t pack = 0;
    for (auto [e, wolf, tr] : w.view<Wolf, lam::Transform>()) {
        (void)e;
        if (wolf.slot < kMaxPack) positions[wolf.slot] = tr.position;
        pack = std::max<std::size_t>(pack, wolf.slot + 1u);
    }
    const std::span<const lam::Vec2> pack_positions(positions.data(), std::min<std::size_t>(pack, kMaxPack));

    for (auto [e, wolf, tr, motor, route] : w.view<Wolf, lam::Transform, lam::TopDownMotor, lam::PathRoute>()) {
        const lam::Vec2 feet = tr.position;
        if (wolf.cooldown > 0) --wolf.cooldown;
        wolf.lunging = false;
        wolf.wander_step = false;
        drive(motor, {}, motor.max_speed);                      // stand still unless a state moves

        const lam::Vec2 eye = lam::sight_point(body_at(feet, t.wolf_box));
        const bool sees = player->alive && lam::can_see(*tiles, eye, target_eye, t.wolf_sight);
        lam::Fsm<WolfState> &fsm = wolf.fsm;
        const int k = static_cast<int>(fsm.ticks_in_state);    // ticks since entering this state

        switch (fsm.state) {
        case WolfState::Wander: {
            if (sees) {
                fsm.set(WolfState::Chase);
                wolf.unseen = 0;
                route = lam::PathRoute{};
                break;
            }
            if (wolf.timer > 0) {
                --wolf.timer;
                // The straight path is checked from where the wolf stands.
                if (wolf.timer == 0 &&
                    !lam::pick_wander_goal(*tiles, foot_box(t.wolf_box), feet, wolf.home, t.wolf_wander_radius, rng,
                                           wolf.goal, 4)) {
                    wolf.timer = rng.between(30, 90);
                }
                break;
            }
            if (lam::distance(feet, wolf.goal) < 3.0f) {
                wolf.timer = rng.between(40, 120);
                break;
            }
            const lam::Vec2 v = lam::seek(feet, wolf.goal, t.wolf_wander_speed);
            drive(motor, v, t.wolf_wander_speed);
            wolf.facing = facing_for(v, wolf.facing);
            wolf.wander_step = true;                            // wolf_bites checks it moved
            break;
        }
        case WolfState::Chase: {
            if (!player->alive) {
                fsm.set(WolfState::Wander);
                wolf.timer = 60;
                break;
            }
            wolf.unseen = sees ? 0 : wolf.unseen + 1;
            if (wolf.unseen > t.wolf_lose_ticks) {
                fsm.set(WolfState::Wander);
                wolf.timer = 30;
                break;
            }
            const float d = lam::distance(feet, target);
            if (sees && wolf.cooldown == 0 && d < t.wolf_lunge_range) {
                fsm.set(WolfState::Telegraph);
                wolf.dir = lam::normalize_or(target - feet, facing_vector(wolf.facing));
                wolf.facing = facing_for(wolf.dir, wolf.facing);
                ctx.emit(WolfWindup{e});
                break;
            }
            lam::Vec2 v;
            if (lam::box_path_clear(*tiles, foot_box(t.wolf_box), feet, target)) {
                // In the open: approach, but hold a ring while waiting for the cooldown.
                const float ring = wolf.cooldown > 0 ? t.wolf_ring : 0.0f;
                v = lam::hold_ring(feet, target, ring, speed, speed * 0.6f, wolf.slot % 2 == 0 ? 1 : -1);
            } else {
                // The search starts at the cell of the body centre (start_offset) and the wolf
                // steers that same point through cell centres.
                const lam::Vec2 centre_offset{0.0f, -t.wolf_box.y * 0.5f};
                const lam::IVec2 goal = standing_tile(*tiles, target, t.player_box);
                // Pending requests (served next tick, or later without budget) are not repeated.
                if (!w.has<lam::PathRequest>(e) &&
                    lam::route_stale(route, goal, now, route.computed_tick,
                                     static_cast<std::uint32_t>(t.wolf_repath_ticks))) {
                    ctx.commands().add(e, lam::PathRequest{.goal = goal, .requested_tick = now,
                                                           .start_offset = centre_offset});
                }
                v = lam::follow_route(route, *tiles, feet + centre_offset, 3.0f) * speed;
            }
            // The pure span form of separation: only the pack pushes, never the traveller.
            v += lam::separation(wolf.slot, pack_positions, t.wolf_separation_radius, t.wolf_separation_strength);
            v = lam::clamp_length(v, speed);
            drive(motor, v, speed);
            wolf.facing = facing_for(v, wolf.facing);
            break;
        }
        case WolfState::Telegraph:
            // It stands and aims; the last aim_lock ticks keep their direction, so a late dash dodges.
            if (k <= t.wolf_telegraph - t.wolf_aim_lock) {
                wolf.dir = lam::normalize_or(target - feet, wolf.dir);
                wolf.facing = facing_for(wolf.dir, wolf.facing);
            }
            if (k >= t.wolf_telegraph) {
                fsm.set(WolfState::Lunge);
                wolf.hit_this_lunge = false;
            }
            break;
        case WolfState::Lunge:
            drive(motor, wolf.dir * t.wolf_lunge_speed, t.wolf_lunge_speed);
            wolf.facing = facing_for(wolf.dir, wolf.facing);
            wolf.lunging = true;                                // the bite is checked after the move
            if (k >= t.wolf_lunge) fsm.set(WolfState::Recover);
            break;
        case WolfState::Recover:
            if (k >= t.wolf_recover) {
                fsm.set(WolfState::Chase);
                wolf.cooldown = t.wolf_cooldown;
            }
            break;
        }
        fsm.tick();
    }
}

void wolf_bites(lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    const Tuning &t = tuning(ctx);
    const lam::TileWorld *tiles = ctx.try_local<lam::TileWorld>();
    lam::Entity pe;
    Player *player = w.single_or_null<Player>(&pe);
    if (tiles == nullptr || player == nullptr) return;
    const lam::Vec2 target = w.get<lam::Transform>(pe).position;
    const lam::Circle hurt{{target.x, target.y - 8.0f}, 6.0f};  // where the traveller can be bitten
    lam::Rng &rng = rules_rng(ctx);
    RunState &run = ctx.local<RunState>();

    for (auto [e, wolf, tr, prev] : w.view<Wolf, lam::Transform, lam::PrevTransform>()) {
        (void)e;
        const float moved = lam::distance(prev.position, tr.position);
        wolf.moving = moved > 0.05f;
        if (wolf.wander_step && moved < 0.01f) wolf.timer = rng.between(40, 120);   // stuck: pause
        if (!wolf.lunging || wolf.hit_this_lunge || !player->vulnerable()) continue;
        const lam::Vec2 f = tr.position;
        const lam::Circle bite{{f.x + wolf.dir.x * 6.0f, f.y - 6.0f + wolf.dir.y * 6.0f}, 8.0f};
        // Never through a tree: the bite needs a clear line too.
        if (!lam::overlaps(bite, hurt) ||
            !lam::tile_ray_clear(*tiles, lam::sight_point(body_at(f, t.wolf_box)),
                                 lam::sight_point(body_at(target, t.player_box)))) {
            continue;
        }
        wolf.hit_this_lunge = true;
        --player->hearts;
        player->protection = t.hurt_protection;
        player->knockback = wolf.dir * t.knockback_speed;
        ctx.emit(PlayerHurt{target, player->hearts});
        if (player->hearts <= 0) {
            player->alive = false;
            run.phase = Phase::Over;
            run.over_at = w.tick();
            ++run.deaths;
            run.best = std::max(run.best, run.collected);
            ctx.emit(PlayerDied{target, run.collected});
            // run_if freezes the rules from the next tick, but not the engine's motors: without
            // this the traveller and a lunging wolf would coast on through the game-over screen.
            stop_actors(w);
        }
    }
}

} // namespace lantern
