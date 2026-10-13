// Platformer: the game skeleton of docs/RECIPES.md grown into a two-screen level.
//   platformer                                   play: arrows/WASD + Space/Z (pad: stick + South)
//   platformer --bot                             watch the autoplay bot (an InputDriver); it closes the
//                                                result screen itself, so the level loops
//   platformer --headless --bot --frames 3000    no window; prints one summary line at exit
//   platformer --bot --hidden --no-vsync --frames 1200 --perf    frame-time summary at exit
//   (engine flags --seed S, --hash-every N, --record-input F, --replay F work as usual)
// Presentation only. Rules, level spawner and bot are in platformer_sim (src/platformer/).
#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>
#include <lam/assets/asset_server.h>
#include <lam/audio/audio.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/log.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/driver.h>
#include <lam/particles/particles.h>
#include <lam/physics/tile_world.h>
#include <lam/render/camera.h>
#include <lam/render/sprite.h>
#include <lam/scene/scene.h>
#include <lam/ui/menu_scenes.h>
#include <lam/ui/ui.h>
#include "platformer/bot.h"
#include "platformer/rules.h"

using namespace lam;
using namespace plat;

namespace {
// ---- Presentation-only resources (not snapshotted, not hashed) ----------------------------------
struct RunMode { bool headless_bot = false; };                  // App resource
// --perf: real frame times (Time::real_seconds deltas; frame_dt is the tick length under --step),
// reserved up front so sampling never allocates.
struct PerfLog {
    static constexpr std::size_t kWarmupFrames = 60;           // startup, asset loads, first fade
    std::vector<float> ms;
    double last_real = -1;
};
struct CameraRig { Entity camera; bool attached = false, bounded = false; };   // World resource
struct Hud {                                                    // World resource
    UiId coins = 0, lives = 0, score = 0;
    std::int32_t shown_coins = -1, shown_total = -1, shown_lives = -1, shown_score = -1;
};

// ---- Scenes ----------------------------------------------------------------------------------
// The level is a Scene with its own World and asset scope. The result text scene goes on top as
// an overlay that blocks its ticks and input.
class LevelScene final : public Scene {
public:
    std::string_view name() const override { return "level"; }
    void enter(App &app, World &world) override {
        spawn_level(app, world);                                // sim: Level resource + Tilemap entity
        Entity cam = world.create();
        world.add<Camera2D>(cam);
        world.add<CameraFollow>(cam, CameraFollow{.half_life = 0.12f, .dead_zone = {-24, -32, 48, 56}});
        world.add<CameraBounds>(cam);                           // filled once TileWorld is known
        world.resources().insert<CameraRig>(CameraRig{.camera = cam});
        if (UiTree *ui = world.resources().try_get<UiTree>()) {   // HUD: one row of labels
            const UiId bar = ui->panel(0, Rect{4, 4, 300, 14}, UiLayout::Horizontal);
            Hud hud;
            hud.coins = ui->label(bar, "COINS 0/0");
            hud.lives = ui->label(bar, "LIVES 3");
            hud.score = ui->label(bar, "SCORE 0");
            world.resources().insert<Hud>(hud);
        }
    }
    // The text scene on top was dismissed: start the level again.
    void resumed(App &app, const SceneMessage *msg) override {
        (void)msg;
        app.res<SceneStack>().replace(std::make_unique<LevelScene>(), Transition{.kind = TransitionKind::Fade});
    }
};

// ---- Presentation systems (all gated on has_local<Level>) ---------------------------------------
// Update -1350, before camera_follow (-1300). The hero is spawned by tilemap_sync once the map is
// Ready, so the camera picks its target and the map bounds up here, not in enter().
void camera_attach(Ctx &ctx) {
    CameraRig *rig = ctx.try_local<CameraRig>();
    if (rig == nullptr || (rig->attached && rig->bounded)) return;
    World &w = ctx.world();
    if (CameraFollow *follow = w.try_get<CameraFollow>(rig->camera); follow != nullptr && !rig->attached) {
        for (auto [e, tf] : w.view<Transform>(With<Player>{})) {
            follow->target = e;
            follow->snap_next = true;                           // no glide from (0, 0) on the first frame
            rig->attached = true;
        }
    }
    const TileWorld *tw = ctx.try_local<TileWorld>();
    CameraBounds *bounds = w.try_get<CameraBounds>(rig->camera);
    if (tw != nullptr && bounds != nullptr && tw->shapes.width() > 0) {
        const float ts = static_cast<float>(tw->tile_size);
        bounds->world = Rect{tw->origin.x, tw->origin.y, static_cast<float>(tw->shapes.width()) * ts,
                             static_cast<float>(tw->shapes.height()) * ts};
        bounds->enabled = true;
        rig->bounded = true;
    }
}

// Update -850, before ui_update: only touch labels whose value changed.
void hud_update(Ctx &ctx) {
    Hud *hud = ctx.try_local<Hud>();
    UiTree *ui = ctx.try_local<UiTree>();
    const Level &lv = ctx.local<Level>();
    if (hud == nullptr || ui == nullptr) return;
    char buf[48];
    if (lv.coins != hud->shown_coins || lv.coins_total != hud->shown_total) {
        const int n = std::snprintf(buf, sizeof buf, "COINS %d/%d", static_cast<int>(lv.coins),
                                    static_cast<int>(lv.coins_total));
        ui->set_text(hud->coins, std::string_view(buf, static_cast<std::size_t>(n)));
        hud->shown_coins = lv.coins;
        hud->shown_total = lv.coins_total;
    }
    if (lv.lives != hud->shown_lives) {
        const int n = std::snprintf(buf, sizeof buf, "LIVES %d", static_cast<int>(lv.lives));
        ui->set_text(hud->lives, std::string_view(buf, static_cast<std::size_t>(n)));
        hud->shown_lives = lv.lives;
    }
    if (lv.score != hud->shown_score) {
        const int n = std::snprintf(buf, sizeof buf, "SCORE %d", static_cast<int>(lv.score));
        ui->set_text(hud->score, std::string_view(buf, static_cast<std::size_t>(n)));
        hud->shown_score = lv.score;
    }
}

// Update: blink the hero while invulnerable.
void blink_invulnerable(Ctx &ctx) {
    for (auto [e, st, sprite] : ctx.world().view<PlayerState, Sprite>()) {
        const bool dim = st.invuln_ticks > 0 && (st.invuln_ticks / 4) % 2 == 0;
        sprite.tint.a = dim ? std::uint8_t{70} : std::uint8_t{255};
    }
}

// TickEnd: LevelFinished comes from PostPhysics. Pushing the scene from a tick stage keeps replays
// exact.
void on_level_finished(Ctx &ctx) {
    for (const LevelFinished &f : ctx.read<LevelFinished>()) {
        if (ctx.res<RunMode>().headless_bot) {                  // headless autoplay: done
            ctx.app().quit(0);
            return;
        }
        const Level &lv = ctx.local<Level>();
        char body[160];
        std::snprintf(body, sizeof body, "Coins %d/%d   Stomps %d   Deaths %d\nScore %d\n\nPress Enter to play again",
                      static_cast<int>(lv.coins), static_cast<int>(lv.coins_total), static_cast<int>(lv.stomps),
                      static_cast<int>(lv.deaths), static_cast<int>(lv.score));
        if (SceneStack *stack = ctx.try_res<SceneStack>())
            stack->push(make_text_scene(f.won ? "LEVEL COMPLETE" : "GAME OVER", body));
    }
}

// Update (Once): one sample per rendered frame.
void perf_sample(Ctx &ctx) {
    PerfLog &log = ctx.res<PerfLog>();
    const double now = ctx.time().real_seconds;
    const double last = log.last_real;
    log.last_real = now;
    if (ctx.time().frame >= PerfLog::kWarmupFrames && last >= 0.0 && log.ms.size() < log.ms.capacity())
        log.ms.push_back(static_cast<float>((now - last) * 1000.0));
}

void print_perf(const PerfLog &log) {
    if (log.ms.empty()) return;
    std::vector<float> v = log.ms;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (float x : v) sum += x;
    const double avg = sum / static_cast<double>(v.size());
    const auto pct = [&](double p) { return v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())))]; };
    std::printf("platformer perf: frames=%zu avg_ms=%.3f p50_ms=%.3f p99_ms=%.3f max_ms=%.3f fps=%.0f\n", v.size(), avg,
                static_cast<double>(pct(0.5)), static_cast<double>(pct(0.99)), static_cast<double>(v.back()),
                avg > 0 ? 1000.0 / avg : 0.0);
}

// TickEnd: headless autoplay without --frames still ends (10 simulated minutes).
void headless_watchdog(Ctx &ctx) {
    if (ctx.res<RunMode>().headless_bot && ctx.world().tick() > 36000) ctx.app().quit(2);
}

void add_view_system(App &app, Stage stage, SystemFn fn, std::string_view name, int order) {
    app.add_system({.stage = stage, .fn = fn, .name = name, .order = order, .run_if = &has_local<Level>});
}

// Particle configs built in code (AssetServer::add).
FxIds make_fx(AssetServer &assets) {
    const auto burst = [&](Color from, Color to, float speed, float gravity, std::string_view name) {
        EmitterConfig c;
        c.capacity = 128;
        c.speed_min = speed * 0.4f;
        c.speed_max = speed;
        c.life_min = 0.25f;
        c.life_max = 0.55f;
        c.size_start = 2;
        c.size_end = 0;
        c.color_start = from;
        c.color_end = to;
        c.gravity = {0, gravity};
        return assets.add<EmitterConfig>(sanitize(c), name);
    };
    return FxIds{.coin = burst(Color::hex(0xFFD54AFF), Color::hex(0xFFFFFF00), 70, 160, "fx/coin"),
                 .stomp = burst(Color::hex(0xB6F2A0FF), Color::hex(0x6AC46A00), 60, 240, "fx/stomp"),
                 .hurt = burst(Color::hex(0xE74C3CFF), Color::hex(0x34495E00), 90, 200, "fx/hurt"),
                 .win = burst(Color::hex(0xFFFFFFFF), Color::hex(0x2ECC7100), 110, 60, "fx/win")};
}

// Startup (main world, root asset scope): open the level scene.
void start(Ctx &ctx) {
    if (SceneStack *stack = ctx.try_res<SceneStack>()) stack->push(std::make_unique<LevelScene>());
}

// The level world after run(): a scene's world is named after Scene::name() and lives until ~App.
const World &level_world(App &app) {
    const World *w = app.find_world(str_id("level"));
    return w != nullptr ? *w : app.main_world();
}
}  // namespace

int main(int argc, char **argv) {
    PlatformerBot bot;                                          // outlives the App (driver is not owned)
    App app({.title = "Hop Hill", .virtual_size = {480, 270}}, argc, argv);
    app.add_plugins(DefaultPlugins{.render = {.clear_color = Color::hex(0x8ECAE6FF)}});
    app.add_plugin<PlatformerPlugin>();                         // sim: components, events, controls, rules
    const bool use_bot = app.options().has("--bot");
    app.insert_resource<RunMode>(RunMode{.headless_bot = use_bot && app.options().headless});

    // Prefab names match the Tiled object types in maps/level1.tmj.
    if (Status s = app.res<PrefabLibrary>().load("prefabs.json", app.serialize_context()); !s) {
        LAM_LOG_ERROR("{}", s.error().message);
        return 1;
    }
    AssetServer &assets = app.res<AssetServer>();
    app.insert_resource<FxIds>(make_fx(assets));
    if (Audio *audio = app.try_res<Audio>()) audio->add_bank(assets.load<SoundBank>("sfx/platformer.sounds.json"));
    if (use_bot) {
        bot.set_controls(app.res<Controls>());
        bot.set_close_result(!app.options().headless);          // windowed: a demo loop that restarts
        app.res<InputSystem>().set_driver(&bot);
    }

    const bool perf = app.options().has("--perf") && !app.options().headless;   // headless frame_dt is synthetic
    if (perf) {
        app.insert_resource<PerfLog>().ms.reserve(1u << 16);
        app.add_system({.stage = Stage::Update, .fn = perf_sample, .name = "platformer.perf", .order = 990,
                        .scope = SystemScope::Once});
    }

    app.add_startup(start, "platformer.start");
    add_view_system(app, Stage::Update, camera_attach, "platformer.camera_attach", -1350);
    add_view_system(app, Stage::Update, blink_invulnerable, "platformer.blink", -1340);
    add_view_system(app, Stage::Update, hud_update, "platformer.hud", -850);
    add_view_system(app, Stage::TickEnd, on_level_finished, "platformer.level_finished", 0);
    add_view_system(app, Stage::TickEnd, headless_watchdog, "platformer.watchdog", 10);

    const int code = app.run();
    if (app.options().headless) std::printf("%s\n", summary_line(level_world(app)).c_str());   // bot or --replay
    if (perf) print_perf(app.res<PerfLog>());
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
