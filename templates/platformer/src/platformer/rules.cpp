// The rules. Where each one runs in a tick (engine systems in brackets):
//   Input        player_input     input -> PlatformerMotor intent
//   Simulate     level_start      record the respawn point and the coin count, once
//   [Physics]    the motor moves the hero against tiles; contacts become events
//   PostPhysics  touch_triggers   coins and the goal (TriggerEntered, same tick)
//                hazards          spikes (TileHazardTouched) and pits (below kill_y)
// Effects go out as PlaySound / BurstAt events. destroy and teleport go through ctx.commands()
// because they happen inside event loops and views.
#include <cinttypes>
#include <cstdio>
#include <lam/assets/asset_server.h>
#include <lam/audio/components.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/keys.h>
#include <lam/physics/components.h>
#include <lam/physics/events.h>
#include <lam/physics/motors.h>
#include <lam/tilemap/tilemap.h>
#include "platformer/game.h"

namespace platformer {
using namespace lam;
using namespace lam::literals;                                  // "coin"_id
namespace {

void sparkle(const Ctx &ctx, Vec2 at, std::uint16_t count) {
    if (const Fx *fx = ctx.try_res<Fx>(); fx != nullptr && fx->sparkle)
        ctx.emit(BurstAt{.config = fx->sparkle, .position = at, .count = count});
}

// The input frame is neutral while another scene has focus. The bot fills the same frame.
void player_input(Ctx &ctx) {
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    const bool frozen = ctx.local<Level>().won;
    const Vec2 axis = frozen ? Vec2{} : in.axis2(c.move);
    for (auto [e, motor] : ctx.world().view<PlatformerMotor>(With<Player>{})) {
        motor.move_x = axis.x;                                  // tuning is in prefabs.json
        motor.drop = axis.y > 0.5f;
        motor.climb_y = axis.y;                                 // ladders: tiles with "ladder": true
        motor.jump_pressed = !frozen && in.is_pressed(c.jump);
        motor.jump_held = !frozen && in.is_held(c.jump);        // released early = shorter jump
    }
}

// The hero is spawned from the map's objects once the map is Ready, so the first tick that sees
// it records the start. Outside the map counts as solid, so a pit needs its own kill line.
void level_start(Ctx &ctx) {
    Level &lv = ctx.local<Level>();
    const TileWorld *tw = ctx.try_local<TileWorld>();
    if (lv.started || tw == nullptr || tw->shapes.height() == 0) return;
    World &w = ctx.world();
    for (auto [e, tf] : w.view<Transform>(With<Player>{})) {
        lv.respawn = tf.position;
        lv.started = true;
    }
    if (!lv.started) return;
    lv.coins_total = static_cast<std::int32_t>(w.count<Coin>());
    lv.kill_y = tw->origin.y + static_cast<float>((tw->shapes.height() - 1) * tw->tile_size) - 6.0f;
}

void touch_triggers(Ctx &ctx) {
    World &w = ctx.world();
    Level &lv = ctx.local<Level>();
    for (const TriggerEntered &t : ctx.read<TriggerEntered>()) {  // trigger first, then the body
        if (!w.has<Player>(t.other) || !w.alive(t.trigger)) continue;
        const Vec2 at = w.get<Transform>(t.trigger).position;
        if (w.has<Coin>(t.trigger)) {
            ++lv.coins;                                         // TODO: scoring, combos, extra lives
            ctx.commands().destroy(t.trigger);
            ctx.emit(PlaySound{.group = "coin"_id, .position = at});   // group in sfx/game.sounds.json
            sparkle(ctx, at, 12);
        } else if (w.has<Goal>(t.trigger) && !lv.won) {
            lv.won = true;
            sparkle(ctx, at + Vec2{0, -16}, 40);
            ctx.emit(LevelFinished{.won = true});
        }
    }
}

void hazards(Ctx &ctx) {
    World &w = ctx.world();
    Level &lv = ctx.local<Level>();
    if (!lv.started || lv.won) return;
    const auto events = ctx.read<TileHazardTouched>();
    for (auto [e, tf, vel] : w.view<Transform, Velocity>(With<Player>{})) {
        bool hit = tf.position.y > lv.kill_y;
        for (const TileHazardTouched &h : events) hit = hit || h.entity == e;
        if (!hit) continue;
        ++lv.falls;                                             // TODO: lives, game over, checkpoints
        vel.linear = {};
        ctx.commands().teleport(e, lv.respawn);
    }
}

void rule(App &app, Stage stage, SystemFn fn, std::string_view name, int order = 0) {
    // run_if: only on the world holding a Level, never on menu or result-screen worlds.
    app.add_system({.stage = stage, .fn = fn, .name = name, .order = order, .run_if = &has_local<Level>});
}
}  // namespace

void RulesPlugin::build(App &app) {
    app.require_plugin("lam.physics", name());
    app.register_component<Player>().register_component<Coin>().register_component<Goal>();
    app.add_event<LevelFinished>();
    app.add_event<PlaySound>();                                 // tests run without audio and
    app.add_event<BurstAt>();                                   // particles plugins
    Controls c{};                                               // TODO: more actions (attack, dash...)
    if (ActionMap *map = app.try_res<ActionMap>()) {            // absent in pure tests
        const auto k = [](Key key) { return ActionMap::key(key); };
        c.move = map->axis2("move", {{k(Key::W), k(Key::S), k(Key::A), k(Key::D)},
                                     {k(Key::Up), k(Key::Down), k(Key::Left), k(Key::Right)}},
                            {{PadAxis::LeftX, PadAxis::LeftY}});
        c.jump = map->button("jump", {k(Key::Space), k(Key::Z), ActionMap::pad(PadButton::South)});
    }
    app.insert_resource<Controls>(c);
    rule(app, Stage::Input, player_input, "platformer.player_input");
    rule(app, Stage::Simulate, level_start, "platformer.level_start");
    rule(app, Stage::PostPhysics, touch_triggers, "platformer.touch_triggers");
    rule(app, Stage::PostPhysics, hazards, "platformer.hazards", 10);
}

void start_level(App &app, World &world, std::string_view map) {
    world.resources().insert<Level>();
    world.snapshot_resource<Level>();
    const Entity e = world.create();
    world.add<Transform>(e);                                    // map origin = (0, 0)
    world.add<Tilemap>(e, Tilemap{.map = app.res<AssetServer>().load<TilemapAsset>(map)});
}

std::string summary_line(const World &world) {
    const Level *found = world.resources().try_get<Level>();
    const Level lv = found != nullptr ? *found : Level{};
    char buf[160];
    std::snprintf(buf, sizeof buf, "platformer summary: ticks=%" PRIu64 " coins=%d/%d falls=%d won=%d hash=%016" PRIx64,
                  world.tick(), lv.coins, lv.coins_total, lv.falls, lv.won ? 1 : 0, world.state_hash());
    return buf;
}
}
