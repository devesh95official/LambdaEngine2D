// Rule systems of the platformer. Stage map (engine orders in docs/GUIDE.md):
//   Input        player_input        controls -> PlatformerMotor intent (motors run in Physics)
//   Simulate     level_start, lift_setup, enemy_patrol, player_timers
//   Physics      engine only: path_followers/moving_platforms, carry_riders, motors, contact_events
//   PostPhysics  collect_coins, touch_checkpoints, touch_goal, stomp_or_hurt, hazards, player_sfx,
//                keep_score -- they read this tick's Collision*/Trigger*/TileHazardTouched events
//   Animate      engine only: motor_anim_bridge feeds the hero's AnimGraph
// Effects go out as PlaySound/BurstAt events; destroy and teleport go through ctx.commands().
#include <lam/animation/animator.h>
#include <lam/assets/asset_server.h>
#include <lam/audio/components.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/input/actions.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/math/scalar.h>
#include <lam/physics/components.h>
#include <lam/physics/events.h>
#include <lam/physics/motors.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/tilemap.h>
#include "platformer/rules.h"

namespace plat {
using namespace lam;
using namespace lam::literals;                                  // "coin"_id
namespace {

// ---- fx helpers ---------------------------------------------------------------------------------
void sfx(const Ctx &ctx, StrId group, Vec2 at) { ctx.emit(PlaySound{.group = group, .position = at}); }

void fx(const Ctx &ctx, AssetId<EmitterConfig> FxIds::*which, Vec2 at, std::uint16_t count) {
    const FxIds *ids = ctx.try_res<FxIds>();
    if (ids != nullptr && (ids->*which)) ctx.emit(BurstAt{.config = ids->*which, .position = at, .count = count});
}

// Lose a life and respawn at the last checkpoint, or end the run. `ignore_invuln` is for falling
// out of the map, where waiting out the invulnerability would leave the hero stuck in the pit.
void hurt(const Ctx &ctx, Level &lv, Entity hero, bool ignore_invuln = false) {
    World &w = ctx.world();
    PlayerState &st = w.get<PlayerState>(hero);
    if ((st.invuln_ticks > 0 && !ignore_invuln) || lv.complete || lv.game_over) return;
    const Vec2 at = w.get<Transform>(hero).position;
    ++lv.deaths;
    --lv.lives;
    sfx(ctx, "hurt"_id, at);
    fx(ctx, &FxIds::hurt, at + Vec2{0, -7}, 24);
    if (lv.lives <= 0) {
        lv.game_over = true;
        lv.finish_tick = w.tick();
        ctx.emit(LevelFinished{.won = false});
        return;
    }
    st.invuln_ticks = kInvulnTicks;
    ctx.commands().teleport(hero, lv.respawn);                  // deferred: we may be inside a view
    if (Velocity *v = w.try_get<Velocity>(hero)) v->linear = {};
}

// ---- Input -----------------------------------------------------------------------------------
// Input -> motor intent. input_frame() is neutral while an overlay has focus.
void player_input(Ctx &ctx) {
    const Controls *c = ctx.try_res<Controls>();
    const Level &lv = ctx.local<Level>();
    const InputFrame &in = input_frame(ctx);
    const bool frozen = c == nullptr || lv.complete || lv.game_over;
    for (auto [e, motor] : ctx.world().view<PlatformerMotor>(With<Player>{})) {
        const Vec2 mv = frozen ? Vec2{} : in.axis2(c->move);
        motor.move_x = clamp(mv.x, -1.0f, 1.0f);
        motor.climb_y = clamp(mv.y, -1.0f, 1.0f);              // +y is down (world units)
        motor.drop = mv.y > 0.5f;                               // hold down: drop through one-way tiles
        motor.jump_pressed = !frozen && in.is_pressed(c->jump); // latched: exactly one tick per press
        motor.jump_held = !frozen && in.is_held(c->jump);       // release early -> jump_cut
    }
}

// ---- Simulate ------------------------------------------------------------------------------
// Objects are spawned by tilemap_sync in FrameBegin, so the first tick that sees the hero records
// the start point, counts coins and derives the fall-out line from the TileWorld size.
void level_start(Ctx &ctx) {
    Level &lv = ctx.local<Level>();
    const TileWorld &tw = ctx.local<TileWorld>();
    if (lv.started || tw.shapes.height() == 0) return;
    World &w = ctx.world();
    for (auto [e, tf] : w.view<Transform>(With<Player>{})) {
        lv.respawn = tf.position;
        lv.started = true;
    }
    if (!lv.started) return;
    lv.coins_total = static_cast<std::int32_t>(w.count<Coin>());
    lv.kill_y = tw.origin.y + static_cast<float>(tw.shapes.height() - 1) * static_cast<float>(tw.tile_size) - 6.0f;
}

// A lift's path is per object: its Tiled object carries the custom float properties "travel_x" /
// "travel_y" (px, relative to the spawn point), read through the MapObjectRef that spawn_objects
// adds. PathFollower points are absolute world pixels, so the rule converts them once.
void lift_setup(Ctx &ctx) {
    const AssetServer *assets = ctx.try_res<AssetServer>();
    if (assets == nullptr) return;
    World &w = ctx.world();
    for (auto [e, pf, ref, tf] : w.view<PathFollower, MapObjectRef, Transform>()) {
        if (pf.count != 0) continue;
        const MapObject *obj = map_object(w, *assets, ref);
        if (obj == nullptr) continue;
        const Vec2 travel{prop_float(obj->props, "travel_x"_id), prop_float(obj->props, "travel_y"_id)};
        pf.points[0] = tf.position;
        pf.points[1] = tf.position + travel;
        pf.count = 2;
        pf.next = 1;
        pf.forward = true;
    }
}

// Enemies reuse PlatformerMotor: the rule only writes intent (move_x) and reads motor state.
void enemy_patrol(Ctx &ctx) {
    const TileWorld &tw = ctx.local<TileWorld>();
    for (auto [e, motor, patrol, tf, col] : ctx.world().view<PlatformerMotor, Patrol, Transform, Collider>()) {
        const float dir = patrol.dir < 0 ? -1.0f : 1.0f;
        bool turn = (dir < 0 && motor.on_wall_left) || (dir > 0 && motor.on_wall_right);
        if (!turn && motor.on_ground) {                         // ledge or spikes ahead: turn around
            const float front = tf.position.x + col.offset.x + dir * (col.half.x + 2.0f);
            const IVec2 below = tw.tile_at({front, tf.position.y + 2.0f});
            const IVec2 ahead = tw.tile_at({front, tf.position.y - 2.0f});
            turn = tw.at(below) == TileShape::Empty || (tw.flags_at(ahead) & kTileHazard) != 0;
        }
        if (turn) patrol.dir = static_cast<std::int8_t>(-patrol.dir);
        motor.move_x = patrol.dir < 0 ? -1.0f : 1.0f;
    }
}

void player_timers(Ctx &ctx) {
    for (auto [e, st] : ctx.world().view<PlayerState>())
        if (st.invuln_ticks > 0) --st.invuln_ticks;
}

// ---- PostPhysics: react to this tick's contact events ----------------------------------------
void collect_coins(Ctx &ctx) {
    World &w = ctx.world();
    Level &lv = ctx.local<Level>();
    for (const TriggerEntered &t : ctx.read<TriggerEntered>()) {
        if (!w.has<Coin>(t.trigger) || !w.has<Player>(t.other)) continue;
        const Vec2 at = w.get<Transform>(t.trigger).position;
        ++lv.coins;
        ctx.commands().destroy(t.trigger);
        sfx(ctx, "coin"_id, at);
        fx(ctx, &FxIds::coin, at, 12);
    }
}

void touch_checkpoints(Ctx &ctx) {
    World &w = ctx.world();
    Level &lv = ctx.local<Level>();
    for (const TriggerEntered &t : ctx.read<TriggerEntered>()) {
        Checkpoint *cp = w.try_get<Checkpoint>(t.trigger);
        if (cp == nullptr || cp->active || !w.has<Player>(t.other)) continue;
        cp->active = true;
        lv.respawn = w.get<Transform>(t.trigger).position;
        // Animator state is hashed, so the clip switch belongs in the rules.
        Animator *anim = w.try_get<Animator>(t.trigger);
        const AssetServer *assets = ctx.try_res<AssetServer>();
        if (anim != nullptr && assets != nullptr)
            if (const SpriteSheet *sheet = assets->get(anim->sheet)) play(*anim, *sheet, "on"_id);
        sfx(ctx, "checkpoint"_id, lv.respawn);
    }
}

void touch_goal(Ctx &ctx) {
    World &w = ctx.world();
    Level &lv = ctx.local<Level>();
    for (const TriggerEntered &t : ctx.read<TriggerEntered>()) {
        if (!w.has<Goal>(t.trigger) || !w.has<Player>(t.other) || lv.complete || lv.game_over) continue;
        lv.complete = true;
        lv.finish_tick = w.tick();
        const Vec2 at = w.get<Transform>(t.trigger).position;
        sfx(ctx, "win"_id, at);
        fx(ctx, &FxIds::win, at + Vec2{0, -24}, 40);
        ctx.emit(LevelFinished{.won = true});                   // the executable shows the scene
    }
}

// Hero vs enemy: landing on it (falling, feet near its top) stomps it; any other contact hurts.
void stomp_or_hurt(Ctx &ctx) {
    World &w = ctx.world();
    Level &lv = ctx.local<Level>();
    for (const CollisionStarted &c : ctx.read<CollisionStarted>()) {
        const Entity hero = w.has<Player>(c.a) ? c.a : c.b;
        const Entity enemy = other(c, hero);
        if (!w.has<Player>(hero) || !w.has<Patrol>(enemy)) continue;
        const Vec2 n = normal_from(c, hero);                    // from the hero toward the enemy
        const Transform &ht = w.get<Transform>(hero);
        const Transform &et = w.get<Transform>(enemy);
        const Collider &ec = w.get<Collider>(enemy);
        const float vy = w.get<Velocity>(hero).linear.y;
        const float enemy_top = et.position.y + ec.offset.y - ec.half.y;
        const bool from_above = n.y > 0.5f || (vy >= 0 && ht.position.y <= enemy_top + 7.0f);
        if (!from_above) {
            hurt(ctx, lv, hero);
            continue;
        }
        ++lv.stomps;
        ctx.commands().destroy(enemy);
        w.get<Velocity>(hero).linear.y = -kStompBounce;         // motor integrates it next tick
        sfx(ctx, "stomp"_id, et.position);
        fx(ctx, &FxIds::stomp, et.position + Vec2{0, -4}, 16);
    }
}

void hazards(Ctx &ctx) {
    World &w = ctx.world();
    Level &lv = ctx.local<Level>();
    for (const TileHazardTouched &h : ctx.read<TileHazardTouched>())
        if (w.has<Player>(h.entity)) hurt(ctx, lv, h.entity);
    for (auto [e, tf] : w.view<Transform>(With<Player>{}))
        if (tf.position.y > lv.kill_y) hurt(ctx, lv, e, true);
}

void player_sfx(Ctx &ctx) {
    for (auto [e, motor, st, vel, tf] :
         ctx.world().view<PlatformerMotor, PlayerState, Velocity, Transform>(With<Player>{})) {
        if (st.was_grounded && !motor.on_ground && vel.linear.y < -50.0f) sfx(ctx, "jump"_id, tf.position);
        st.was_grounded = motor.on_ground;
    }
}

void keep_score(Ctx &ctx) {
    Level &lv = ctx.local<Level>();
    lv.score = score_of(lv);
}

void add_rule(App &app, Stage stage, SystemFn fn, std::string_view name, int order) {
    // run_if: only worlds that hold a Level (the level scene's world, or main in tests).
    app.add_system({.stage = stage, .fn = fn, .name = name, .order = order, .run_if = &has_local<Level>});
}
}  // namespace

void PlatformerPlugin::build(App &app) {
    app.require_plugin("lam.physics", name());
    app.require_plugin("lam.tilemap", name());
    app.require_plugin("lam.input", name());
    app.register_component<Player>().register_component<Coin>().register_component<Goal>();
    app.register_component<Patrol>().register_component<Checkpoint>();
    app.register_component<PlayerState>();
    app.add_event<LevelFinished>();
    app.add_event<BurstAt>();                                   // idempotent (particles registers it too)
    app.add_event<PlaySound>();                                 // idempotent (audio registers it too)

    // Controls: names resolved to ids once; ActionMap persists rebinds in SettingsStore.
    Controls c{};
    if (ActionMap *map = app.try_res<ActionMap>()) {
        const auto k = [](Key key) { return ActionMap::key(key); };
        c.move = map->axis2("move",
                            {{k(Key::W), k(Key::S), k(Key::A), k(Key::D)},
                             {k(Key::Up), k(Key::Down), k(Key::Left), k(Key::Right)}},
                            {{PadAxis::LeftX, PadAxis::LeftY}});
        c.jump = map->button("jump", {k(Key::Space), k(Key::Z), ActionMap::pad(PadButton::South)});
    }
    app.insert_resource<Controls>(c);

    add_rule(app, Stage::Input, player_input, "platformer.player_input", 0);
    add_rule(app, Stage::Simulate, level_start, "platformer.level_start", -100);
    add_rule(app, Stage::Simulate, lift_setup, "platformer.lift_setup", -50);
    add_rule(app, Stage::Simulate, enemy_patrol, "platformer.enemy_patrol", 0);
    add_rule(app, Stage::Simulate, player_timers, "platformer.player_timers", 10);
    add_rule(app, Stage::PostPhysics, collect_coins, "platformer.collect_coins", 0);
    add_rule(app, Stage::PostPhysics, touch_checkpoints, "platformer.touch_checkpoints", 10);
    add_rule(app, Stage::PostPhysics, touch_goal, "platformer.touch_goal", 20);
    add_rule(app, Stage::PostPhysics, stomp_or_hurt, "platformer.stomp_or_hurt", 30);
    add_rule(app, Stage::PostPhysics, hazards, "platformer.hazards", 40);
    add_rule(app, Stage::PostPhysics, player_sfx, "platformer.player_sfx", 50);
    add_rule(app, Stage::PostPhysics, keep_score, "platformer.keep_score", 100);
}
}
