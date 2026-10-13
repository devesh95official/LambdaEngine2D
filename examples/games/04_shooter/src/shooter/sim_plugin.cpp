// The rules plugin: components, events, actions and tick systems (see rules.h for the stage map).
#include <shooter/sim_plugin.h>

#include <lam/audio/components.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/keys.h>
#include <lam/particles/components.h>
#include <shooter/arena.h>
#include <shooter/components.h>
#include <shooter/crowd.h>
#include <shooter/rules.h>

namespace shooter {
using namespace lam;

namespace {
// Every rule system runs only in worlds holding the Arena resource (menu worlds have none).
void add_rule(App &app, Stage stage, SystemFn fn, std::string_view name, int order) {
    app.add_system(SystemDesc{
        .stage = stage, .fn = fn, .name = name, .order = order, .run_if = &has_local<Arena>});
}
}

void ShooterSimPlugin::build(App &app) {
    app.require_plugin("lam.input", name());
    app.require_plugin("lam.physics", name());
    // Reflected types self-register at static init; registering here documents the set.
    app.register_component<Player>();
    app.register_component<Health>();
    app.register_component<Enemy>();
    app.register_component<Weapon>();
    app.register_component<Bullet>();
    app.register_component<Pickup>();
    // BurstAt/PlaySound are registered here too so the rules work without the particles/audio
    // plugins; reserve_event raises their capacity for whole crowds dying at once.
    app.add_event<EnemyKilled>(EventScope::World, 512);
    app.add_event<PlayerHurt>();
    app.add_event<WaveStarted>();
    app.add_event<WaveCleared>();
    app.add_event<GameOver>();
    app.add_event<PickupTaken>();
    app.add_event<BurstAt>();
    app.add_event<PlaySound>();
    app.reserve_event<BurstAt>(512);
    app.reserve_event<PlaySound>(512);

    // Actions resolve to small ids once; systems test bits, never strings.
    ActionMap &map = app.res<ActionMap>();
    app.insert_resource<Controls>(Controls{
        .move = map.axis2("move",
                          {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A),
                            ActionMap::key(Key::D)}},
                          {{PadAxis::LeftX, PadAxis::LeftY}}),
        .aim = map.axis2("aim",
                         {{ActionMap::key(Key::Up), ActionMap::key(Key::Down),
                           ActionMap::key(Key::Left), ActionMap::key(Key::Right)}},
                         {{PadAxis::RightX, PadAxis::RightY}}),
        .fire = map.button("fire", {ActionMap::mouse(MouseButton::Left), ActionMap::key(Key::Space),
                                    ActionMap::pad(PadButton::RB), ActionMap::pad(PadButton::RT)}),
        .pause = map.button("pause",
                            {ActionMap::key(Key::Escape), ActionMap::pad(PadButton::Start)})});

    add_rule(app, Stage::Input, player_input, "shooter.player_input", 0);
    add_rule(app, Stage::Simulate, tick_timers, "shooter.tick_timers", 0);
    add_rule(app, Stage::Simulate, wave_director, "shooter.wave_director", 10);
    add_rule(app, Stage::Simulate, fire_weapons, "shooter.fire_weapons", 30);
    add_rule(app, Stage::PostPhysics, bullet_hits, "shooter.bullet_hits", 0);
    add_rule(app, Stage::PostPhysics, contact_damage, "shooter.contact_damage", 10);
    add_rule(app, Stage::PostPhysics, collect_pickups, "shooter.collect_pickups", 20);
    add_rule(app, Stage::PostPhysics, enemy_steering, "shooter.enemy_steering", 30);
}
}
