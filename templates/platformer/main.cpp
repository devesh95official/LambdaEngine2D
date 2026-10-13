// platformer: entry point and presentation. Rules, level setup and the bot are in src/platformer/.
//   platformer                                    play: arrows/WASD + Space/Z (pad: stick + South)
//   platformer --bot                              watch the autoplay bot
//   platformer --headless --bot --frames 3000     no window; prints one summary line at exit
#include <cstdio>
#include <memory>
#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/core/default_plugins.h>
#include <lam/core/fixed_string.h>
#include <lam/core/log.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/particles/particles.h>
#include <lam/physics/components.h>
#include <lam/render/camera.h>
#include <lam/render/sprite.h>
#include <lam/scene/scene.h>
#include <lam/ui/menu_scenes.h>
#include <lam/ui/ui.h>
#include "platformer/game.h"

using namespace lam;

namespace {
// Presentation state, not snapshotted.
struct View {                                                   // World resource of the level world
    Entity camera;
    bool camera_ready = false;
    UiId hud = 0;
    FixedString<48> hud_text;
};

// A fresh World and asset scope per run, so "play again" just replaces the scene.
// TODO: a title screen, more levels via a level index.
class LevelScene final : public Scene {
public:
    std::string_view name() const override { return "level"; }
    void enter(App &app, World &world) override {
        platformer::start_level(app, world);                    // sim: Level resource + the map
        View view;
        view.camera = world.create();
        world.add<Camera2D>(view.camera);
        world.add<CameraFollow>(view.camera, CameraFollow{.half_life = 0.12f, .dead_zone = {-24, -32, 48, 56}});
        world.add<CameraBounds>(view.camera);
        if (UiTree *ui = world.resources().try_get<UiTree>())   // TODO: more HUD labels here
            view.hud = ui->label(ui->panel(0, Rect{4, 4, 240, 14}, UiLayout::Horizontal), "");
        world.resources().insert<View>(view);
    }
    void resumed(App &app, const SceneMessage *) override {    // the result screen was closed
        app.res<SceneStack>().replace(std::make_unique<LevelScene>(), Transition{.kind = TransitionKind::Fade});
    }
};

// Update -1350, before camera_follow. The hero is a map object and only exists once the map is
// Ready, so the camera finds its target and bounds here.
void camera_attach(Ctx &ctx) {
    View &view = ctx.local<View>();
    const TileWorld *tiles = ctx.try_local<TileWorld>();
    if (view.camera_ready || tiles == nullptr || tiles->shapes.width() == 0) return;
    World &w = ctx.world();
    for (auto [hero, tf] : w.view<Transform>(With<Player>{})) {
        CameraFollow &follow = w.get<CameraFollow>(view.camera);
        follow.target = hero;
        follow.snap_next = true;
        const float ts = static_cast<float>(tiles->tile_size);
        w.get<CameraBounds>(view.camera) = CameraBounds{
            .world = Rect{tiles->origin.x, tiles->origin.y, static_cast<float>(tiles->shapes.width()) * ts,
                          static_cast<float>(tiles->shapes.height()) * ts},
            .enabled = true};
        view.camera_ready = true;
    }
}

// Update: face the way the hero moves.
void face_direction(Ctx &ctx) {
    for (auto [e, sprite, vel] : ctx.world().view<Sprite, Velocity>(With<Player>{})) {
        if (vel.linear.x < -1) sprite.flags = static_cast<std::uint8_t>(sprite.flags | kFlipX);
        if (vel.linear.x > 1) sprite.flags = static_cast<std::uint8_t>(sprite.flags & ~kFlipX);
    }
}

// Update -850, before ui_update. Only touches the label when the text changes.
void hud_update(Ctx &ctx) {
    View &view = ctx.local<View>();
    UiTree *ui = ctx.try_local<UiTree>();
    if (ui == nullptr || view.hud == 0) return;
    const Level &lv = ctx.local<Level>();
    FixedString<48> text;
    format_to(text, "COINS {}/{}   FALLS {}", lv.coins, lv.coins_total, lv.falls);
    if (text == view.hud_text) return;
    ui->set_text(view.hud, text.view());
    view.hud_text = text;
}

// TickEnd. Pushed from a tick stage, so the result screen shows up the same way in a replay.
void on_level_finished(Ctx &ctx) {
    const bool headless = ctx.app().options().headless;
    if (headless && ctx.world().tick() > 36000) ctx.app().quit(2);   // 10 minutes without a finish
    for (const LevelFinished &f : ctx.read<LevelFinished>()) {
        if (headless) {
            ctx.app().quit(0);
            return;
        }
        const Level &lv = ctx.local<Level>();
        char body[96];
        std::snprintf(body, sizeof body, "Coins %d/%d   Falls %d\n\nPress Enter to play again", lv.coins,
                      lv.coins_total, lv.falls);
        ctx.res<SceneStack>().push(make_text_scene(f.won ? "LEVEL CLEAR" : "GAME OVER", body));
    }
}

// The one particle effect, built in code.
AssetId<EmitterConfig> make_sparkle(AssetServer &assets) {
    EmitterConfig c;                                            // TODO: more effects (dust, hits)
    c.capacity = 128;
    c.speed_min = 30;
    c.speed_max = 90;
    c.life_min = 0.25f;
    c.life_max = 0.5f;
    c.color_start = Color::hex(0xFFE066FF);
    c.color_end = Color::hex(0xFFFFFF00);
    c.gravity = {0, 160};
    return assets.add<EmitterConfig>(sanitize(c), "fx/sparkle");
}

void start(Ctx &ctx) { ctx.res<SceneStack>().push(std::make_unique<LevelScene>()); }

void view_system(App &app, Stage stage, SystemFn fn, std::string_view name, int order) {
    app.add_system({.stage = stage, .fn = fn, .name = name, .order = order, .run_if = &has_local<Level>});
}

// The level world is a scene world, not the main world.
const World &level_world(App &app) {
    for (const WorldEntry &e : app.world_stack())
        if (e.world->resources().has<Level>()) return *e.world;
    return app.main_world();
}
}  // namespace

int main(int argc, char **argv) {
    platformer::Bot bot;                                        // before the App: drivers are not owned
    App app({.title = "platformer", .virtual_size = {480, 270}}, argc, argv);   // TODO: your title
    app.add_plugins(DefaultPlugins{.render = {.clear_color = Color::hex(0x8ECAE6FF)}});
    app.add_plugin<platformer::RulesPlugin>();                  // after DefaultPlugins: it reads ActionMap

    // Prefab names match the object types in maps/level1.tmj: "hero", "coin", "goal".
    if (Status s = app.res<PrefabLibrary>().load("prefabs.json", app.serialize_context()); !s) {
        LAM_LOG_ERROR("{}", s.error().message);
        return 1;
    }
    AssetServer &assets = app.res<AssetServer>();
    app.insert_resource<Fx>(Fx{.sparkle = make_sparkle(assets)});
    if (Audio *audio = app.try_res<Audio>()) audio->add_bank(assets.load<SoundBank>("sfx/game.sounds.json"));
    if (app.options().has("--bot")) {
        bot.controls = app.res<Controls>();
        app.res<InputSystem>().set_driver(&bot);
    }

    app.add_startup(start, "platformer.start");
    view_system(app, Stage::Update, camera_attach, "platformer.camera_attach", -1350);
    view_system(app, Stage::Update, face_direction, "platformer.face_direction", 0);
    view_system(app, Stage::Update, hud_update, "platformer.hud", -850);
    view_system(app, Stage::TickEnd, on_level_finished, "platformer.level_finished", 0);

    const int code = app.run();
    if (app.options().headless) std::printf("%s\n", platformer::summary_line(level_world(app)).c_str());
    if (InputSystem *input = app.try_res<InputSystem>()) input->set_driver(nullptr);
    return code;
}
