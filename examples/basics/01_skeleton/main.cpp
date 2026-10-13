#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/log.h>
#include <lam/ecs/component.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/physics/motors.h>
#include <lam/render/camera.h>
#include <lam/tilemap/tilemap.h>

struct Player {};
LAM_REFLECT_TAG(Player)
struct Controls { lam::AxisAction move; lam::ButtonAction jump; };
using namespace lam;

static void spawn(Ctx &ctx) {                                    // startup: main world, root asset scope
    World &w = ctx.world();
    Entity map = w.create();
    w.add<Transform>(map);
    w.add<Tilemap>(map, Tilemap{.map = ctx.res<AssetServer>().load<TilemapAsset>("maps/level1.tmj")});
    Entity hero = ctx.res<PrefabLibrary>().spawn(w, str_id("hero"), {64, 64});
    w.add<Player>(hero);
    Entity cam = w.create();
    w.add<Camera2D>(cam);
    w.add<CameraFollow>(cam, CameraFollow{.target = hero, .half_life = 0.1f, .dead_zone = {-16, -24, 32, 48}});
}

static void drive(Ctx &ctx) {                                    // Stage::Input, every tick
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    for (auto [e, motor] : ctx.world().view<PlatformerMotor>(With<Player>{})) {
        motor.move_x = in.axis2(c.move).x;
        motor.jump_pressed = in.is_pressed(c.jump);
        motor.jump_held = in.is_held(c.jump);
    }
}

int main(int argc, char **argv) {
    App app({.title = "Hop", .virtual_size = {480, 270}}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.register_component<Player>();
    ActionMap &map = app.res<ActionMap>();
    app.insert_resource<Controls>(Controls{
        .move = map.axis2("move", {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A),
                                    ActionMap::key(Key::D)}}, {{PadAxis::LeftX, PadAxis::LeftY}}),
        .jump = map.button("jump", {ActionMap::key(Key::Space), ActionMap::pad(PadButton::South)})});
    if (Status s = app.res<PrefabLibrary>().load("prefabs.json", app.serialize_context()); !s) {
        LAM_LOG_ERROR("{}", s.error().message);
        return 1;
    }
    app.add_startup(spawn, "spawn");
    app.add_system(Stage::Input, drive, "drive");
    return app.run();
}
