// Top-down shooter executable: plugins, drawn map, camera, HUD, effects, --bot. Rules are in
// src/game/.
//   top_down_shooter                         play: WASD to move, mouse to aim, click/Space to fire
//   top_down_shooter --bot                   watch the bot play
//   top_down_shooter --headless --bot --frames 3600 --seed 7     no window; one summary line
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/fixed_string.h>
#include <lam/core/log.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/sprite.h>
#include <lam/tilemap/tilemap.h>
#include <lam/ui/ui.h>
#include <game/game.h>

using namespace lam;

namespace {
struct Hud { UiId score = 0, status = 0; };                     // World resource

// Startup: the run, then the view.
void setup(Ctx &ctx) {
    World &w = ctx.world();
    const Entity player = game::start_run(ctx.app(), w);
    // Draw-only map; collision comes from game::kArena.
    const Entity map = w.create();
    w.add<Transform>(map);
    const auto arena = ctx.res<AssetServer>().load<TilemapAsset>("maps/arena.tmj");
    w.add<Tilemap>(map, Tilemap{.map = arena, .build_collision = false, .spawn_objects = false});
    const Entity cam = w.create();
    w.add<Camera2D>(cam);
    w.add<CameraFollow>(cam, CameraFollow{.target = player, .half_life = 0.12f});
    constexpr float kW = game::kArenaW * game::kTile, kH = game::kArenaH * game::kTile;
    w.add<CameraBounds>(cam, CameraBounds{.world = Rect{0, 0, kW, kH}, .enabled = true});
    if (UiTree *ui = w.resources().try_get<UiTree>()) {
        const UiId panel = ui->panel(0, Rect{4, 4, 260, 26}, UiLayout::Vertical);
        const UiId score = ui->label(panel, "");
        w.resources().insert<Hud>(Hud{.score = score, .status = ui->label(panel, "")});
    }
}

// Update: HUD text and the hurt blink.
void present(Ctx &ctx) {
    World &w = ctx.world();
    const game::Run &run = ctx.local<game::Run>();
    UiTree &ui = ctx.local<UiTree>();
    const Entity p = game::find_player(w);
    const game::Health *h = p ? w.try_get<game::Health>(p) : nullptr;
    FixedString<64> text;
    format_to(text, "SCORE {}   HP {}   BEST {}", run.score, h ? std::max(h->hp, 0) : 0, run.best);
    ui.set_text(ctx.local<Hud>().score, text.view());
    ui.set_text(ctx.local<Hud>().status, run.over ? "GAME OVER - press FIRE to restart" : "");
    if (Sprite *s = p ? w.try_get<Sprite>(p) : nullptr; s != nullptr && h != nullptr) {
        const bool faded = run.over || (h->invuln / 4) % 2 == 1;
        s->tint = kWhite.with_alpha(static_cast<std::uint8_t>(faded ? 80 : 255));
        // Face the aim. Done here because the animators rewrite kFlipX every tick.
        const bool left = w.get<game::Gun>(p).aim.x < 0;
        s->flags = static_cast<std::uint8_t>(left ? (s->flags | kFlipX) : (s->flags & ~kFlipX));
    }
}

// Effects built in code (AssetServer::add, Ready at once). The rules only see the ids in game::Fx.
// TODO(fx): more emitters and sound groups.
void load_fx(App &app) {
    AssetServer &assets = app.res<AssetServer>();
    const EmitterConfig pop{.capacity = 512, .speed_min = 30, .speed_max = 120,
                            .life_min = 0.2f, .life_max = 0.5f, .size_start = 3,
                            .color_start = Color::hex(0xFFD54AFF),
                            .color_end = Color::hex(0xFF400000), .drag = 2};
    app.insert_resource<game::Fx>(game::Fx{.pop = assets.add(sanitize(pop), "fx/pop")});
    SoundBank bank{.groups = {{.id = game::kSndPop,
                               .variants = {assets.load<SoundClip>("audio/pop.wav")},
                               .volume = 0.6f}}};
    if (Audio *audio = app.try_res<Audio>()) audio->add_bank(assets.add(std::move(bank), "sounds"));
}
}

int main(int argc, char **argv) {
    game::Bot bot;                                              // outlives the App
    App app({.title = "Top-Down Shooter", .virtual_size = {480, 270}}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.add_plugin<game::RulesPlugin>();
    load_fx(app);
    // One bad entry fails the whole file.
    if (Status s = app.res<PrefabLibrary>().load("prefabs.json", app.serialize_context()); !s) {
        LAM_LOG_ERROR("prefabs.json: {}", s.error().message);
        return 1;
    }
    app.add_startup(setup, "top_down_shooter.setup");
    app.add_system({.stage = Stage::Update, .fn = present, .name = "top_down_shooter.present",
                    .run_if = &has_local<Hud>});
    if (app.options().has("--bot")) app.res<InputSystem>().set_driver(&bot);
    const int code = app.run();
    if (app.options().headless) {
        World &w = app.main_world();
        const game::Run *run = w.resources().try_get<game::Run>();
        const game::Run r = run ? *run : game::Run{};
        std::printf("top_down_shooter summary: ticks=%" PRIu64 " score=%d best=%d kills=%u"
                    " deaths=%u enemies=%zu hash=%016" PRIx64 "\n",
                    app.tick(), r.score, r.best, r.kills, r.deaths, w.count<game::Enemy>(),
                    w.state_hash());
    }
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
