// Apple Trail: a snake-style game.
// The traveller walks the orchard picking apples; every apple joins the trail rolling behind.
// Bump a wall or your own trail and the run is over.
//
// Keys: arrows / WASD turn (two quick turns are both remembered), Enter / Space start and play
// again, P / Esc pause.
// Flags: --bot (the BFS bot plays, as an InputDriver, and the title is skipped), plus the engine's
// --headless --frames N --seed S --hash-every N --record-input F --replay F. `--headless --bot`
// prints one summary line; `--bot --replay F` replays a run recorded with `--bot --record-input F`.
// --perf prints frames, ticks and the mean frame time at exit (Release measurements).
#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/log.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/keys.h>
#include <lam/particles/particles.h>
#include <lam/scene/scene.h>
#include <lam/tilemap/tilemap.h>
#include "present.h"
#include "scenes.h"
#include "trail_bot.h"
#include "trail_rules.h"

using namespace lam;

namespace {

// The board: the native map through the engine's TilemapAsset loader (load_now: synchronous, so
// the first scene can build the run at once). The plain-text board is the fallback; the same
// loader reads it as raw ASCII rows ('#' = solid wall).
Result<trail::Orchard> load_board(AssetServer &assets, std::string_view path) {
    LAM_TRY_ASSIGN(const AssetId<TilemapAsset> map, assets.load_now<TilemapAsset>(path));
    const TilemapAsset *asset = assets.get(map);
    if (asset == nullptr) return Error{ErrorCode::Invalid, std::string(path) + ": not ready"};
    return trail::orchard_from_map(*asset, path);
}

Result<trail::Orchard> load_orchard(AssetServer &assets) {
    Result<trail::Orchard> orchard = load_board(assets, "levels/orchard.map.json");
    if (orchard) return orchard;
    LAM_LOG_WARN("{}; falling back to levels/orchard.txt", orchard.error().message);
    return load_board(assets, "levels/orchard.txt");
}

EmitterConfig burst_config(Color start, Color end, float speed_max, float size) {
    EmitterConfig c;
    c.capacity = 256;
    c.speed_min = 30;
    c.speed_max = speed_max;
    c.life_min = 0.3f;
    c.life_max = 0.7f;
    c.size_start = size;
    c.color_start = start;
    c.color_end = end;
    c.gravity = {0, 160};
    c.drag = 1.5f;
    return c;
}

// Startup (main world, root asset scope): art, effects, the board and the first scenes.
void load_game(Ctx &ctx) {
    App &app = ctx.app();
    AssetServer &assets = ctx.res<AssetServer>();
    Result<trail::Orchard> orchard = load_orchard(assets);
    if (!orchard) {
        LAM_LOG_ERROR("{}", orchard.error().message);
        app.quit(1);
        return;
    }
    app.insert_resource<game::GameArt>(game::GameArt{.orchard = std::move(*orchard),
                                                     .traveller = assets.load<SpriteSheet>("sprites/player.json")});
    // Runtime assets (AssetServer::add): Ready at once, ids handed to the rules for BurstAt.
    app.insert_resource<trail::TrailFx>(trail::TrailFx{
        .red_burst = assets.add(burst_config({220, 50, 40, 255}, {255, 255, 255, 0}, 80, 3), "fx/red_burst"),
        .golden_burst = assets.add(burst_config({255, 222, 90, 255}, {255, 255, 255, 0}, 110, 3), "fx/golden_burst"),
        .dust = assets.add(burst_config({200, 190, 170, 255}, {120, 110, 100, 0}, 90, 4), "fx/dust")});

    SceneStack &stack = ctx.res<SceneStack>();
    stack.push(std::make_unique<game::PlayScene>());
    if (!app.options().has("--bot")) stack.push(std::make_unique<game::TitleScene>());   // bot: straight in
}

} // namespace

int main(int argc, char **argv) {
    std::unique_ptr<trail::TrailBot> bot;                       // outlives the App (InputSystem keeps a pointer)
    App app({.title = "Apple Trail", .virtual_size = game::kVirtual, .audio = false}, argc, argv);
    app.add_plugins(DefaultPlugins{.render = {.clear_color = game::kHudBg}});
    app.add_plugin<trail::TrailRulesPlugin>();
    app.add_plugin<game::FlowPlugin>();
    app.add_plugin<game::PresentPlugin>();
    app.insert_resource<game::Session>();

    ActionMap &map = app.res<ActionMap>();
    const trail::Controls controls{
        .up = map.button("up", {ActionMap::key(Key::Up), ActionMap::key(Key::W), ActionMap::pad(PadButton::DUp)}),
        .down = map.button("down", {ActionMap::key(Key::Down), ActionMap::key(Key::S), ActionMap::pad(PadButton::DDown)}),
        .left = map.button("left", {ActionMap::key(Key::Left), ActionMap::key(Key::A), ActionMap::pad(PadButton::DLeft)}),
        .right = map.button("right", {ActionMap::key(Key::Right), ActionMap::key(Key::D), ActionMap::pad(PadButton::DRight)}),
        .confirm = map.button("confirm", {ActionMap::key(Key::Enter), ActionMap::key(Key::Space),
                                          ActionMap::pad(PadButton::South)}),
        .pause = map.button("pause", {ActionMap::key(Key::P), ActionMap::key(Key::Escape), ActionMap::pad(PadButton::Start)})};
    app.insert_resource<trail::Controls>(controls);

    // --bot skips the title and installs the bot. With --replay, the engine's ReplayDriver (set
    // by InputPlugin) feeds the recorded presses instead, so `--bot --replay run.lamr` replays a
    // run recorded with `--bot --record-input run.lamr` to the same hash.
    const bool bot_run = app.options().has("--bot");
    if (bot_run && app.options().replay.empty()) {
        bot = std::make_unique<trail::TrailBot>(controls, 90);  // 90 ticks on "BUMP!"
        app.res<InputSystem>().set_driver(bot.get());
    }
    app.add_startup(load_game, "apple_trail.load");
    const int code = app.run();
    if (bot) app.res<InputSystem>().set_driver(nullptr);
    if (app.options().has("--perf")) {                         // measurement line (any mode)
        const Time &t = app.time();
        const double ms = t.frame > 0 ? t.real_seconds * 1000.0 / static_cast<double>(t.frame) : 0.0;
        std::printf("apple_trail perf: frames=%" PRIu64 " ticks=%" PRIu64 " seconds=%.3f ms_per_frame=%.3f fps=%.0f\n",
                    t.frame, app.tick(), t.real_seconds, ms, ms > 0 ? 1000.0 / ms : 0.0);
    }

    if (app.options().headless && bot_run) {
        const game::Session &s = app.res<game::Session>();
        std::printf("apple_trail summary: ticks=%" PRIu64 " score=%d runs=%d crashes=%d apples=%d total_score=%d "
                    "last_score=%d hash=%016" PRIx64 "\n",
                    app.tick(), s.best, s.runs, s.crashes, s.apples, s.total_score, s.last_score, s.last_hash);
    }
    return code;
}
