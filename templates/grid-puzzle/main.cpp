// Grid Puzzle: a LambdaEngine2D starter template (Sokoban-style). Push every crate onto a goal.
//   grid_puzzle                                   play: WASD/arrows, Z undo (hold), R restart, Enter next
//   grid_puzzle --bot                             the solver plays (an InputDriver)
//   grid_puzzle --headless --bot --frames 3000 --seed 7   no window; prints one summary line
// Presentation and wiring only; the rules are in src/grid_puzzle (grid_puzzle_sim).
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/fixed_string.h>
#include <lam/core/log.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/world.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/ui/ui.h>
#include "grid_puzzle/rules.h"

using namespace lam;
using namespace lam::literals;
using namespace grid_puzzle;

namespace {

// sprites/tiles.png is one row of 16x16 frames, in this order (tools/gen_assets.py draws them).
enum Frame : int { kFloor, kWall, kGoal, kCrate, kCrateOnGoal, kPlayer };
struct Art { AssetId<Texture> sheet; };                         // App resource
struct Hud { UiId label = 0; FixedString<64> shown; };          // World resource; presentation only

TextureRegion frame(const Art &art, Frame f) {
    const float s = static_cast<float>(kTile);
    return {.texture = art.sheet, .source = {static_cast<float>(f) * s, 0, s, s}, .size = {s, s}};
}

// Startup. There are no scenes, so the main world is the game.
// TODO(scenes): a title screen or pause menu is a Scene (RECIPES.md 2.9); systems are already
// gated on has_local<Run>, so they stay off a menu's world.
void start(Ctx &ctx) {
    World &w = ctx.world();
    load_level(w, ctx.res<Levels>().list.front(), 0);
    // Made before the first undo snapshot, so undo never removes the camera.
    w.add<Camera2D>(w.create());
    UiTree &ui = ctx.local<UiTree>();
    const UiId bar = ui.panel(0, Rect{4, 4, 312, 16}, UiLayout::Horizontal);
    w.resources().insert<Hud>(Hud{.label = ui.label(bar, "")});
}

// Update: sprites, camera and HUD follow the rules' state.
void present(Ctx &ctx) {
    World &w = ctx.world();
    const Art &art = ctx.res<Art>();
    const Run &run = ctx.local<Run>();
    const Level &level = ctx.res<Levels>().list[static_cast<std::size_t>(run.level)];
    // GridPosition already holds the destination while the glide plays, so a crate lights up
    // only once its GridMover has arrived.
    const auto look = [&](Entity e, IVec2 cell) {
        const GridMover *m = w.try_get<GridMover>(e);
        const bool on_goal = level.goal(cell) && (m == nullptr || !m->moving);
        return frame(art, w.has<Player>(e) ? kPlayer : on_goal ? kCrateOnGoal : kCrate);
    };
    // Pieces spawned by the rules (level start, restart, undo) get their Sprite here. Sprite isn't
    // hashed, so a frame stage may add it.
    for (auto [e, gp] : w.view<GridPosition>(Exclude<Sprite>{}))
        ctx.commands().add(e, Sprite{.region = look(e, gp.cell), .pivot = {0.5f, 0.5f}});
    for (auto [e, gp, sprite] : w.view<GridPosition, Sprite>()) sprite.region = look(e, gp.cell);
    const Vec2 middle{static_cast<float>(level.cells.width() * kTile) * 0.5f,
                      static_cast<float>(level.cells.height() * kTile) * 0.5f - 6};   // room for the HUD
    for (auto [e, cam] : w.view<Camera2D>()) cam.position = middle;
    // TODO(ui): the HUD. Only set_text when the text changes.
    Hud &hud = ctx.local<Hud>();
    FixedString<64> text;
    if (run.solved) format_to(text, "SOLVED IN {} MOVES!  ENTER: NEXT LEVEL", run.moves);
    else format_to(text, "LEVEL {}  MOVES {}  PUSHES {}   Z UNDO  R RESTART", run.level + 1, run.moves, run.pushes);
    if (text != hud.shown) {
        ctx.local<UiTree>().set_text(hud.label, text.view());
        hud.shown = text;
    }
}

// Render: the static board, under the sprites.
// TODO(art): for bigger boards, build a Tilemap (tilemap/tilemap.h) and let the engine draw it.
void draw_board(Ctx &ctx) {
    const Art &art = ctx.res<Art>();
    const Level &level = ctx.res<Levels>().list[static_cast<std::size_t>(ctx.local<Run>().level)];
    RenderQueue &q = ctx.res<RenderQueue>();
    for (int y = 0; y < level.cells.height(); ++y) {
        for (int x = 0; x < level.cells.width(); ++x) {
            const Frame f = level.wall({x, y}) ? kWall : level.goal({x, y}) ? kGoal : kFloor;
            q.region(frame(art, f), to_vec2(IVec2{x, y} * kTile), {1, 1}, 0, {0, 0}, kWhite, kLayerTiles, 0);
        }
    }
}

// The rules get effect ids through the optional Fx resource.
void load_effects(App &app) {
    AssetServer &assets = app.res<AssetServer>();
    EmitterConfig dust;                                         // TODO(fx): or a .particles.json file
    dust.capacity = 64;
    dust.speed_min = 15;
    dust.speed_max = 45;
    dust.life_min = 0.2f;
    dust.life_max = 0.45f;
    dust.size_start = 3;
    dust.color_start = Color{214, 196, 160, 255};
    dust.color_end = Color{214, 196, 160, 0};
    app.insert_resource<Fx>(Fx{.dust = assets.add(sanitize(dust), "fx/dust")});
    if (Audio *audio = app.try_res<Audio>()) {                  // one group per PlaySound name
        SoundBank bank;
        bank.groups.push_back({.id = "push"_id, .variants = {assets.load<SoundClip>("sfx/push.wav")}, .bus = "sfx"_id});
        audio->add_bank(assets.add(std::move(bank), "sfx/game"));
    }
}

}  // namespace

int main(int argc, char **argv) {
    std::unique_ptr<SolverBot> bot;                             // declared first: it outlives the App
    // TODO(rename): the title also names the settings folder (user_data_dir).
    App app({.title = "Grid Puzzle", .virtual_size = {320, 180}}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.add_plugin<RulesPlugin>();

    Result<std::string> text = app.res<AssetServer>().read_text("levels.txt");
    Result<std::vector<Level>> levels = text ? parse_levels(*text) : Result<std::vector<Level>>(text.error());
    if (!levels) {
        LAM_LOG_ERROR("levels.txt: {}", levels.error().message);
        return 1;
    }
    app.insert_resource<Levels>(Levels{std::move(*levels)});
    app.insert_resource<Art>(Art{.sheet = app.res<AssetServer>().load<Texture>("sprites/tiles.png")});
    load_effects(app);

    app.add_startup(start, "grid_puzzle.start");
    app.add_system({.stage = Stage::Update, .fn = present, .name = "grid_puzzle.present", .run_if = &has_local<Hud>});
    app.add_system({.stage = Stage::Render, .fn = draw_board, .name = "grid_puzzle.board", .run_if = &has_local<Hud>});

    if (app.options().has("--bot")) {                           // unknown flags land in options().extra
        bot = std::make_unique<SolverBot>(app.res<Controls>());
        app.res<InputSystem>().set_driver(bot.get());
    }
    const int code = app.run();
    if (app.options().headless) {                               // one machine-readable line for scripts
        const Run &run = app.main_world().resources().get<Run>();
        std::printf("grid_puzzle summary: ticks=%" PRIu64 " level=%d cleared=%d moves=%d hash=%016" PRIx64 "\n",
                    app.tick(), run.level + 1, run.cleared, run.moves, app.main_world().state_hash());
    }
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
