// LanternRunSimPlugin: registers the rules. The schedule (stage, order) is listed in sim/rules.h;
// `--print-schedule` prints it live. Rule systems carry run_if = is_playing, so the title, the
// game-over screen and App::set_paused (pause) all freeze the forest without extra checks.
#include "sim/plugin.h"

#include <lam/ecs/world.h>

#include "sim/components.h"
#include "sim/rules.h"

namespace lantern {

void LanternRunSimPlugin::build(lam::App &app) {
    app.register_component<Player>();
    app.register_component<Wolf>();
    app.register_component<Lantern>();
    app.register_component<Tree>();
    app.register_component<Rock>();
    app.add_event<LanternCollected>();
    app.add_event<PlayerDashed>();
    app.add_event<PlayerHurt>();
    app.add_event<PlayerDied>();
    app.add_event<WaveUp>();
    app.add_event<WolfWindup>();
    if (app.try_res<Tuning>() == nullptr) app.insert_resource<Tuning>();
    app.insert_resource<SimOptions>(options_);

    using lam::Stage;
    app.add_startup(setup_forest, "lantern.setup_forest");
    app.add_system({.stage = Stage::Simulate, .fn = player_rules, .name = "lantern.player_rules", .order = 0,
                    .run_if = is_playing});
    app.add_system({.stage = Stage::Simulate, .fn = wolf_brains, .name = "lantern.wolf_brains", .order = 10,
                    .run_if = is_playing});
    app.add_system(Stage::Simulate, run_phase, "lantern.run_phase", 100);
    app.add_system({.stage = Stage::PostPhysics, .fn = lantern_pickups, .name = "lantern.lantern_pickups",
                    .order = 0, .run_if = is_playing});
    app.add_system({.stage = Stage::PostPhysics, .fn = wolf_bites, .name = "lantern.wolf_bites", .order = 10,
                    .run_if = is_playing});
    app.add_system(Stage::PostPhysics, choose_clips, "lantern.choose_clips", 20);
    app.add_system(Stage::Animate, face_sprites, "lantern.face_sprites", 0);
}

void LanternRunSimPlugin::init_world(lam::App &, lam::World &world) {
    world.resources().insert<RunState>();
    world.snapshot_resource<RunState>();                        // undo, saves and state_hash cover it
}

} // namespace lantern
