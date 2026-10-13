// Roguelike starter: presentation and wiring. The rules are in src/game (roguelike_sim).
//   roguelike                                            play: arrows/WASD move or attack, Space waits
//   roguelike --bot                                      watch the bot play
//   roguelike --headless --bot --frames 3000 --seed 7    no window; prints "roguelike summary: ..."
// Frame stages here only write presentation state. UI that changes the game queues an Intent
// (RECIPES.md 2.10).
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/core/default_plugins.h>
#include <lam/core/log.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/turns.h>
#include <lam/input/driver.h>
#include <lam/particles/components.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/render_queue.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/ui/ui.h>
#include <game/rules.h>

using namespace lam;
using namespace lam::literals;

namespace {
struct Fx { AssetId<EmitterConfig> spark; };    // App resource
struct Hud { UiId status = 0, message = 0; };   // World resource

// Startup: the run, then camera and HUD.
void start(Ctx &ctx) {
    World &w = ctx.world();
    game::start_run(w, ctx.app().seed()); // --seed S picks the dungeon
    const Entity cam = w.create();
    w.add<Camera2D>(cam);
    w.add<CameraFollow>(cam, CameraFollow{.target = w.resources().get<game::RunState>().player});
    w.add<CameraShake>(cam);
    if (UiTree *ui = w.resources().try_get<UiTree>()) { // TODO(ui): pack, message log, minimap
        const UiId top = ui->panel(0, Rect{4, 2, 472, 12}, UiLayout::Horizontal);
        const UiId bottom = ui->panel(0, Rect{4, 256, 472, 12}, UiLayout::Horizontal);
        w.resources().insert<Hud>(Hud{.status = ui->label(top, ""), .message = ui->label(bottom, "")});
    }
}

void set_text(UiTree &ui, UiId id, std::string_view text) { // skip no-ops, set_text re-lays out
    if (const UiNode *n = ui.get(id); n != nullptr && n->text.view() != text) ui.set_text(id, text);
}

// Update: camera on the current hero (each run spawns a new one), hide monsters out of view, HUD.
void present(Ctx &ctx) {
    World &w = ctx.world();
    const game::RunState &run = ctx.local<game::RunState>();
    for (auto [e, follow] : w.view<CameraFollow>())
        if (follow.target != run.player) {
            follow.target = run.player;
            follow.snap_next = true; // don't glide across the map
        }
    for (auto [e, sprite, pos] : w.view<Sprite, GridPosition>(With<game::FloorLocal>{}))
        sprite.flags = game::cell_visible(w, pos.cell) ? static_cast<std::uint8_t>(sprite.flags & ~kHidden)
                                                      : static_cast<std::uint8_t>(sprite.flags | kHidden);
    UiTree *ui = ctx.try_local<UiTree>();
    const Hud *hud = ctx.try_local<Hud>();
    const game::Stats *st = w.try_get<game::Stats>(run.player);
    if (ui == nullptr || hud == nullptr || st == nullptr) return;
    FixedString<64> line;
    format_to(line, "HP {}/{}   Depth {}   Kills {}   Score {}", st->hp, st->max_hp, run.depth, run.kills,
              run.score);
    set_text(*ui, hud->status, line.view());
    set_text(*ui, hud->message, run.message.view());
}

// Update: Hit events become sparks, plus camera shake when the hero is hurt. The rules already
// emitted the sound.
void hit_fx(Ctx &ctx) {
    World &w = ctx.world();
    const TileWorld &tiles = ctx.local<TileWorld>();
    const Fx &fx = ctx.res<Fx>();
    for (const game::Hit &hit : ctx.read<game::Hit>()) {
        ctx.emit(BurstAt{.config = fx.spark, .position = tiles.tile_center(hit.cell),
                         .count = static_cast<std::uint16_t>(hit.killed ? 24 : 10)});
        if (hit.on_player)
            for (auto [e, shake] : w.view<CameraShake>()) shake.trauma = std::min(1.0f, shake.trauma + 0.4f);
    }
}

// Render: flat cells under the fog. Unexplored cells are skipped, remembered ones dimmed.
// TODO(art): draw a tileset instead (a Tilemap with build_collision = false, RECIPES.md 2.8).
void draw_dungeon(Ctx &ctx) {
    World &w = ctx.world();
    RenderQueue *q = ctx.try_res<RenderQueue>();
    const TileWorld &tiles = ctx.local<TileWorld>();
    const IVec2 stairs = ctx.local<game::RunState>().stairs;
    if (q == nullptr) return;
    for (int y = 0; y < tiles.shapes.height(); ++y)
        for (int x = 0; x < tiles.shapes.width(); ++x) {
            const IVec2 c{x, y};
            if (!game::cell_explored(w, c)) continue;
            Color col = c == stairs                          ? Color{240, 200, 80, 255}
                        : tiles.at(c) == TileShape::Solid ? Color{92, 86, 108, 255}
                                                          : Color{38, 34, 46, 255};
            if (!game::cell_visible(w, c)) col = lerp(col, kBlack, 0.55f);
            q->rect(tiles.tile_rect(c), col, kLayerTiles);
        }
}

// Two runs with the same --seed must print the same line.
void print_summary(App &app) {
    World &w = app.main_world();
    const game::RunState *run = w.resources().try_get<game::RunState>();
    const game::Stats *st = run != nullptr ? w.try_get<game::Stats>(run->player) : nullptr;
    const TurnScheduler *turns = w.resources().try_get<TurnScheduler>();
    std::printf("roguelike summary: ticks=%" PRIu64 " depth=%d kills=%d score=%d hp=%d dead=%d turns=%" PRIu64
                " hash=%016" PRIx64 "\n",
                app.tick(), run ? run->depth : 0, run ? run->kills : 0, run ? run->score : 0, st ? int{st->hp} : 0,
                run && run->phase == game::Phase::Dead ? 1 : 0, turns ? turns->turns : std::uint64_t{0},
                w.state_hash());
}
} // namespace

int main(int argc, char **argv) {
    static game::BotDriver bot; // must outlive the App; InputSystem doesn't own it
    App app({.title = "Roguelike", .virtual_size = {480, 270}}, argc, argv);
    app.add_plugins(DefaultPlugins{}); // every module; raylib backends are skipped under --headless
    app.add_plugin<game::RulesPlugin>();
    if (Status s = app.res<PrefabLibrary>().load("prefabs.json", app.serialize_context()); !s) {
        LAM_LOG_ERROR("roguelike: {}", s.error().message);
        return 1;
    }
    // Effects. TODO(art): tune the sparks, add sound groups in assets/sfx/game.sounds.json.
    AssetServer &assets = app.res<AssetServer>();
    EmitterConfig spark; // or load a .particles.json
    spark.capacity = 256;
    spark.speed_min = 25;
    spark.speed_max = 80;
    spark.life_min = 0.15f;
    spark.life_max = 0.4f;
    spark.color_start = Color{255, 230, 140, 255};
    spark.color_end = Color{255, 70, 40, 0};
    app.insert_resource<Fx>(Fx{.spark = assets.add<EmitterConfig>(sanitize(spark), "fx/spark")});
    if (Audio *audio = app.try_res<Audio>()) audio->add_bank(assets.load<SoundBank>("sfx/game.sounds.json"));

    app.add_startup(start, "game.start");
    app.add_system({.stage = Stage::Update, .fn = present, .name = "game.present",
                    .run_if = &has_local<game::RunState>});
    app.add_system({.stage = Stage::Update, .fn = hit_fx, .name = "game.hit_fx", .order = 10,
                    .run_if = &has_local<game::RunState>});
    app.add_system({.stage = Stage::Render, .fn = draw_dungeon, .name = "game.draw_dungeon",
                    .run_if = &has_local<game::RunState>});
    if (app.options().has("--bot")) app.res<InputSystem>().set_driver(&bot);
    const int code = app.run();
    if (app.options().headless) print_summary(app);
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
