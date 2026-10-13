// ArenaScene and the pause / game-over menus (engine MenuScene, lam/ui/menu_scenes.h).
#include "present/scenes.h"

#include <utility>
#include <lam/assets/asset_server.h>
#include <lam/audio/components.h>
#include <lam/core/fixed_string.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/render/camera.h>
#include <lam/tilemap/tilemap.h>
#include <lam/ui/menu_scenes.h>
#include <shooter/components.h>
#include <shooter/ids.h>
#include "present/presentation.h"

// The sim library cannot see lam_ui, so the bot hashes the menu protocol id itself; keep it equal.
static_assert(shooter::ids::kIntentMenuSelect == lam::kMenuSelectIntent);

namespace shooter_app {
using namespace lam;
using shooter::Arena;

namespace {
constexpr StrId kResume = str_id("resume");
constexpr StrId kRestart = str_id("restart");
constexpr StrId kQuit = str_id("quit");

// Item 0 is the default action in both menus (the bot selects item 0).
std::unique_ptr<Scene> make_pause_menu() {
    MenuSpec spec;
    spec.title = "PAUSED";
    spec.items.push_back(MenuItem{.id = kResume, .label = "Resume"});
    spec.items.push_back(MenuItem{.id = kRestart, .label = "Restart"});
    spec.items.push_back(MenuItem{.id = kQuit, .label = "Quit"});
    return std::make_unique<MenuScene>(std::move(spec));
}

std::unique_ptr<Scene> make_game_over_menu(const shooter::GameOver &e, std::int64_t best) {
    MenuSpec spec;
    spec.title = "GAME OVER";
    spec.items.push_back(MenuItem{.id = kRestart, .label = "Restart"});
    spec.items.push_back(MenuItem{.id = kQuit, .label = "Quit"});
    MenuItem info{.id = str_id("info"), .kind = UiKind::Label};
    format_to(info.label, "SCORE {}  WAVE {}  BEST {}", e.score, e.wave, best);
    spec.items.push_back(info);
    return std::make_unique<MenuScene>(std::move(spec));
}
}

void ArenaScene::enter(App &app, World &world) {
    const GameFlags &flags = app.res<GameFlags>();
    const Entity player = shooter::setup_arena(app, world, flags.arena);   // rules state + player

    // The Tiled map is drawn only: the rules built the TileWorld from the same layout already.
    // Loaded in this scene's asset scope (current during enter), released when the scene exits.
    const Entity map = world.create();
    world.add<Transform>(map);
    const AssetId<TilemapAsset> asset = app.res<AssetServer>().load<TilemapAsset>("maps/arena.tmj");
    world.add<Tilemap>(map,
                       Tilemap{.map = asset, .build_collision = false, .spawn_objects = false});

    // Camera: follow with a little look-ahead, clamp to the arena, shake on events.
    const Entity cam = world.create();
    world.add<Camera2D>(cam);
    world.add<CameraFollow>(cam, CameraFollow{.target = player,
                                              .half_life = 0.12f,
                                              .lookahead_seconds = 0.15f,
                                              .lookahead_max = 24});
    constexpr float kW = shooter::kArenaW * shooter::kTileSize;
    constexpr float kH = shooter::kArenaH * shooter::kTileSize;
    world.add<CameraBounds>(cam, CameraBounds{.world = Rect{0, 0, kW, kH}, .enabled = true});
    world.add<CameraShake>(cam, CameraShake{.max_offset = 4});
    world.add<AudioListener>(player, AudioListener{.range = 320});

    build_hud(world);
    app.res<Session>().arena = &world;
}

void ArenaScene::resumed(App &app, const SceneMessage *msg) {
    if (msg == nullptr) return;
    if (msg->kind == kQuit) {
        app.quit();
        return;
    }
    const bool over = world().resources().get<Arena>().phase == shooter::Phase::GameOver;
    if (msg->kind == kRestart || over) {
        // Don't touch rule state here; queue an intent for player_input.
        app.res<InputSystem>().queue_intent(Intent{shooter::kIntentRestart});
    }
}

void flow_pause(Ctx &ctx) {
    if (ctx.local<Arena>().phase == shooter::Phase::GameOver) return;
    if (!input_frame(ctx).is_pressed(ctx.res<shooter::Controls>().pause)) return;
    ctx.res<SceneStack>().push(make_pause_menu());
}

void flow_game_over(Ctx &ctx) {
    for (const shooter::GameOver &e : ctx.read<shooter::GameOver>())
        ctx.res<SceneStack>().push(make_game_over_menu(e, ctx.local<Arena>().best));
}
}
