// Invaders: scenes, drawing and particles. Rules are in src/invaders/.
// Left/Right or A/D move, Space/Up/W/click fire, Enter starts, P or Esc pauses.
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <format>
#include <memory>
#include <string_view>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/input/actions.h>
#include <lam/input/keys.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/scene/scene.h>
#include "invaders/rules.h"

using namespace lam;
using namespace inv;

namespace {

constexpr Color kBg{10, 12, 28, 255}, kInk{232, 236, 255, 255}, kDim{120, 130, 175, 255};
constexpr Color kPink{255, 94, 168, 255}, kCyan{94, 224, 255, 255}, kGreen{125, 255, 122, 255};
constexpr Color kYellow{255, 216, 74, 255}, kRed{255, 77, 77, 255}, kShield{61, 220, 132, 255};

// ---- sprites (X = lit) -----------------------------------------------------------------------
constexpr const char *kSquid[2][8] = {
    {"...XX...", "..XXXX..", ".XXXXXX.", "XX.XX.XX", "XXXXXXXX", "..X..X..", ".X.XX.X.", "X.X..X.X"},
    {"...XX...", "..XXXX..", ".XXXXXX.", "XX.XX.XX", "XXXXXXXX", ".X.XX.X.", "X......X", ".X....X."}};
constexpr const char *kCrab[2][8] = {
    {"..X.....X..", "...X...X...", "..XXXXXXX..", ".XX.XXX.XX.", "XXXXXXXXXXX", "X.XXXXXXX.X", "X.X.....X.X",
     "...XX.XX..."},
    {"..X.....X..", "X..X...X..X", "X.XXXXXXX.X", "XXX.XXX.XXX", "XXXXXXXXXXX", ".XXXXXXXXX.", "..X.....X..",
     ".X.......X."}};
constexpr const char *kOcto[2][8] = {
    {"....XXXX....", ".XXXXXXXXXX.", "XXXXXXXXXXXX", "XXX..XX..XXX", "XXXXXXXXXXXX", "...XX..XX...",
     "..XX.XX.XX..", "XX........XX"},
    {"....XXXX....", ".XXXXXXXXXX.", "XXXXXXXXXXXX", "XXX..XX..XXX", "XXXXXXXXXXXX", "..XXX..XXX..",
     ".XX..XX..XX.", "..XX....XX.."}};
constexpr const char *kUfo[7] = {".....XXXXXX.....", "...XXXXXXXXXX...", "..XXXXXXXXXXXX..", ".XX.XX.XX.XX.XX.",
                                 "XXXXXXXXXXXXXXXX", "..XXX..XX..XXX..", "...X........X..."};
constexpr const char *kCannon[8] = {"......X......", ".....XXX.....", ".....XXX.....", ".XXXXXXXXXXX.",
                                    "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX", "XXXXXXXXXXXXX"};

// One rect per run of lit pixels in a row.
void bitmap(RenderQueue &q, const char *const *rows, int h, float x, float y, Color c, std::uint8_t layer) {
    for (int r = 0; r < h; ++r) {
        const std::string_view row = rows[r];
        for (std::size_t i = 0; i < row.size();) {
            if (row[i] != 'X') { ++i; continue; }
            std::size_t j = i;
            while (j < row.size() && row[j] == 'X') ++j;
            q.rect({x + static_cast<float>(i), y + static_cast<float>(r), static_cast<float>(j - i), 1}, c, layer);
            i = j;
        }
    }
}

void label(const Ctx &ctx, std::string_view text, Vec2 pos, float size, Color color, bool centred = false) {
    const RenderDefaults *d = ctx.try_res<RenderDefaults>();
    const Font *font = d != nullptr ? ctx.res<AssetServer>().get(d->font) : nullptr;
    if (font == nullptr) return;
    if (centred) pos.x -= std::floor(measure_text(*font, text, size).x * 0.5f);
    draw_text(ctx.res<RenderQueue>(), *font, text, pos, TextStyle{.size = size, .color = color, .layer = kLayerUi});
}

template <class... A> std::string_view fmt(char (&buf)[64], std::format_string<A...> f, A &&...args) {
    const auto r = std::format_to_n(buf, sizeof buf - 1, f, std::forward<A>(args)...);
    return {buf, static_cast<std::size_t>(r.out - buf)};
}

struct Session {                                                // survives restarts
    int high = 0, runs = 0, last_score = 0, last_wave = 1, last_lives = 0, last_kills = 0;
    std::uint64_t last_hash = 0;
};

// ---- scenes -------------------------------------------------------------------------------------
class PlayScene final : public Scene {
public:
    std::string_view name() const override { return "play"; }
    void enter(App &app, World &world) override {
        start_run(world);
        const Entity cam = world.create();
        world.add<Camera2D>(cam, Camera2D{.position = {kW / 2.0f, kH / 2.0f}});
        world.add<CameraShake>(cam, CameraShake{.max_offset = 4});
        ++app.res<Session>().runs;
    }
};

struct OverTimer { int ticks = 0; };

bool pressed(const Ctx &ctx, ButtonAction Controls::*which) { return input_frame(ctx).is_pressed(ctx.res<Controls>().*which); }

void dim(const Ctx &ctx) { ctx.res<RenderQueue>().rect({0, 0, kW, kH}, {6, 8, 20, 215}, kLayerUi); }

void title_tick(Ctx &ctx) {
    if ((pressed(ctx, &Controls::confirm) || pressed(ctx, &Controls::fire)) && ctx.res<SceneStack>().queued() == 0)
        ctx.res<SceneStack>().pop();
}
void title_draw(Ctx &ctx) {
    char buf[64];
    dim(ctx);
    label(ctx, "INVADERS", {kW / 2.0f, 36}, 24, kYellow, true);
    RenderQueue &q = ctx.res<RenderQueue>();
    bitmap(q, kUfo, 7, kW / 2.0f - 70, 100, kRed, kLayerUi);
    label(ctx, "= ? MYSTERY", {kW / 2.0f - 46, 100}, 8, kInk);
    bitmap(q, kSquid[0], 8, kW / 2.0f - 66, 114, kPink, kLayerUi);
    label(ctx, "= 30 POINTS", {kW / 2.0f - 46, 114}, 8, kInk);
    bitmap(q, kCrab[0], 8, kW / 2.0f - 68, 128, kCyan, kLayerUi);
    label(ctx, "= 20 POINTS", {kW / 2.0f - 46, 128}, 8, kInk);
    bitmap(q, kOcto[0], 8, kW / 2.0f - 68, 142, kGreen, kLayerUi);
    label(ctx, "= 10 POINTS", {kW / 2.0f - 46, 142}, 8, kInk);
    if (static_cast<int>(ctx.time().real_seconds * 2) % 2 == 0)
        label(ctx, "PRESS ENTER OR SPACE", {kW / 2.0f, 172}, 8, kInk, true);
    label(ctx, "ARROWS/AD MOVE   SPACE/CLICK FIRE   P PAUSE", {kW / 2.0f, 192}, 8, kDim, true);
    label(ctx, fmt(buf, "HIGH SCORE {}", ctx.res<Session>().high), {kW / 2.0f, 66}, 8, kDim, true);
}

void pause_tick(Ctx &ctx) {
    if ((pressed(ctx, &Controls::pause) || pressed(ctx, &Controls::confirm)) && ctx.res<SceneStack>().queued() == 0)
        ctx.res<SceneStack>().pop();
}
void pause_draw(Ctx &ctx) {
    dim(ctx);
    label(ctx, "PAUSED", {kW / 2.0f, 100}, 16, kInk, true);
    label(ctx, "P OR ENTER TO GO ON", {kW / 2.0f, 124}, 8, kDim, true);
}

void over_tick(Ctx &ctx) {
    if (++ctx.local<OverTimer>().ticks < 45) return;
    if ((pressed(ctx, &Controls::confirm) || pressed(ctx, &Controls::fire)) && ctx.res<SceneStack>().queued() == 0)
        ctx.res<SceneStack>().reset(std::make_unique<PlayScene>());
}
void over_draw(Ctx &ctx) {
    char buf[64];
    const Session &s = ctx.res<Session>();
    dim(ctx);
    label(ctx, "GAME OVER", {kW / 2.0f, 78}, 24, kRed, true);
    label(ctx, fmt(buf, "SCORE {}   WAVE {}", s.last_score, s.last_wave), {kW / 2.0f, 116}, 8, kInk, true);
    label(ctx, fmt(buf, "HIGH SCORE {}", s.high), {kW / 2.0f, 130}, 8, s.last_score >= s.high ? kYellow : kDim, true);
    if (ctx.local<OverTimer>().ticks >= 45 && static_cast<int>(ctx.time().real_seconds * 2) % 2 == 0)
        label(ctx, "PRESS ENTER TO PLAY AGAIN", {kW / 2.0f, 160}, 8, kInk, true);
}

class TitleScene final : public Scene {
public:
    std::string_view name() const override { return "title"; }
    bool overlay() const override { return true; }
    void enter(App &app, World &) override {
        add_system(app, Stage::Simulate, title_tick, "title.tick");
        add_system(app, Stage::RenderUI, title_draw, "title.draw");
    }
};
class PauseScene final : public Scene {
public:
    std::string_view name() const override { return "pause"; }
    bool overlay() const override { return true; }
    void enter(App &app, World &) override {
        add_system(app, Stage::Simulate, pause_tick, "pause.tick");
        add_system(app, Stage::RenderUI, pause_draw, "pause.draw");
    }
};
class OverScene final : public Scene {
public:
    std::string_view name() const override { return "over"; }
    bool overlay() const override { return true; }
    void enter(App &app, World &world) override {
        world.resources().insert<OverTimer>();
        add_system(app, Stage::Simulate, over_tick, "over.tick");
        add_system(app, Stage::RenderUI, over_draw, "over.draw");
    }
};

// ---- play-world systems ---------------------------------------------------------------------------
void flow(Ctx &ctx) {                                           // runs after the rules
    const Run &r = ctx.local<Run>();
    Session &s = ctx.res<Session>();
    s.high = std::max(s.high, r.score);
    s.last_score = r.score;
    s.last_wave = r.wave;
    s.last_lives = r.lives;
    s.last_kills = r.kills;
    SceneStack &stack = ctx.res<SceneStack>();
    if (stack.queued() > 0) return;
    if (r.over) stack.push(std::make_unique<OverScene>());
    else if (pressed(ctx, &Controls::pause)) stack.push(std::make_unique<PauseScene>());
}

void record_hash(Ctx &ctx) { ctx.res<Session>().last_hash = ctx.world().state_hash(); }   // TickEnd

void draw_world(Ctx &ctx) {                                     // world layers, shaken by the camera
    const Run &r = ctx.local<Run>();
    RenderQueue &q = ctx.res<RenderQueue>();
    const float t = static_cast<float>(ctx.time().real_seconds);
    for (std::uint32_t i = 0; i < 70; ++i) {                    // twinkling stars
        const std::uint32_t h = i * 2654435761u;
        const float x = static_cast<float>(h % kW), y = static_cast<float>((h >> 9) % (kGroundY - 16) + 12);
        const auto a = static_cast<std::uint8_t>(90 + 80 * (0.5f + 0.5f * std::sin(t * 1.7f + static_cast<float>(i))));
        q.rect({x, y, 1, 1}, {150, 160, 220, a}, kLayerBackground);
    }
    q.rect({0, kGroundY, kW, 1}, kShield, kLayerTiles);

    for (int s = 0; s < kShields; ++s)
        for (int c = 0; c < kShieldCells; ++c)
            if (r.shield[static_cast<std::size_t>(s * kShieldCells + c)] != 0)
                q.rect({static_cast<float>(shield_x(s) + (c % kShieldCW) * kShieldCell),
                        static_cast<float>(kShieldY + (c / kShieldCW) * kShieldCell), kShieldCell, kShieldCell},
                       kShield, kLayerTiles);

    const std::size_t frame = static_cast<std::size_t>(r.anim);
    for (int i = 0; i < kCount; ++i) {
        if (r.alive[static_cast<std::size_t>(i)] == 0) continue;
        const int row = i / kCols;
        const float x = static_cast<float>(inv_x(r.fx, i % kCols)), y = static_cast<float>(inv_y(r.fy, row));
        if (row == 0) bitmap(q, kSquid[frame], 8, x + 2, y, kPink, kLayerWorld);
        else if (row < 3) bitmap(q, kCrab[frame], 8, x, y, kCyan, kLayerWorld);
        else bitmap(q, kOcto[frame], 8, x, y, kGreen, kLayerWorld);
    }
    if (r.ufo_active) bitmap(q, kUfo, 7, static_cast<float>(r.ufo_x), kUfoY, kRed, kLayerWorld);

    const float px = static_cast<float>(r.player_x - kPlayerW / 2);
    if (r.dead_ticks == 0) bitmap(q, kCannon, 8, px, kPlayerY, kYellow, kLayerWorld);
    else if ((r.dead_ticks / 6) % 2 == 0 && r.dead_ticks > 40) bitmap(q, kCannon, 8, px, kPlayerY, kRed, kLayerWorld);

    if (r.shot.active) q.rect({static_cast<float>(r.shot.x), static_cast<float>(r.shot.y), 1, 4}, kInk, kLayerWorld);
    const int wiggle = static_cast<int>(ctx.time().tick / 4 % 2);
    for (const Shot &b : r.bombs) {
        if (!b.active) continue;
        const float bx = static_cast<float>(b.x), by = static_cast<float>(b.y);
        if (b.kind == 0) {                                      // zigzag
            for (int k = 0; k < 6; ++k)
                q.rect({bx - 1 + static_cast<float>((k + wiggle) % 2 * 2), by + static_cast<float>(k), 1, 1}, kInk, kLayerWorld);
        } else {                                                // plunger
            q.rect({bx, by, 1, 6}, kYellow, kLayerWorld);
            q.rect({bx - 1, by + static_cast<float>(wiggle ? 1 : 4), 3, 1}, kYellow, kLayerWorld);
        }
    }
}

void draw_hud(Ctx &ctx) {                                       // screen space
    const Run &r = ctx.local<Run>();
    char buf[64];
    label(ctx, fmt(buf, "SCORE {:05}", r.score), {6, 4}, 8, kInk);
    label(ctx, fmt(buf, "HI {:05}", std::max(ctx.res<Session>().high, r.score)), {kW / 2.0f, 4}, 8, kYellow, true);
    label(ctx, fmt(buf, "WAVE {}", r.wave), {kW - 50.0f, 4}, 8, kInk);
    label(ctx, fmt(buf, "{}", std::max(r.lives, 0)), {6, kGroundY + 5.0f}, 8, kInk);
    for (int i = 0; i < r.lives - 1 && i < 5; ++i)
        bitmap(ctx.res<RenderQueue>(), kCannon, 8, 18.0f + static_cast<float>(i) * 17, kGroundY + 5.0f, kYellow, kLayerUi);
    if (r.popup_ticks > 0)
        label(ctx, fmt(buf, "{}", r.popup_value), {static_cast<float>(r.popup_x), kUfoY}, 8, kRed, true);
    if (r.wave_pause > 0) label(ctx, fmt(buf, "WAVE {}", r.wave), {kW / 2.0f, 110}, 16, kYellow, true);
}

class ViewPlugin final : public Plugin {
public:
    std::string_view name() const override { return "invaders.view"; }
    void build(App &app) override {
        constexpr auto has_run = &has_local<Run>;
        app.add_system({.stage = Stage::Simulate, .fn = flow, .name = "invaders.flow", .order = 100, .run_if = has_run});
        app.add_system({.stage = Stage::TickEnd, .fn = record_hash, .name = "invaders.hash", .order = 900, .run_if = has_run});
        app.add_system({.stage = Stage::Render, .fn = draw_world, .name = "invaders.draw", .run_if = has_run});
        app.add_system({.stage = Stage::RenderUI, .fn = draw_hud, .name = "invaders.hud", .run_if = has_run});
    }
};

EmitterConfig burst_config(Color c, float speed_max) {
    EmitterConfig e;
    e.capacity = 512;
    e.speed_min = 20;
    e.speed_max = speed_max;
    e.life_min = 0.25f;
    e.life_max = 0.6f;
    e.size_start = 2;
    e.size_end = 0.5f;
    e.color_start = c;
    e.color_end = {c.r, c.g, c.b, 0};
    e.gravity = {0, 80};
    e.drag = 2.0f;
    return e;
}

void startup(Ctx &ctx) {
    AssetServer &assets = ctx.res<AssetServer>();
    FxIds fx;
    fx.ids[kFxPink] = assets.add(burst_config(kPink, 90), "fx/pink");
    fx.ids[kFxCyan] = assets.add(burst_config(kCyan, 90), "fx/cyan");
    fx.ids[kFxGreen] = assets.add(burst_config(kGreen, 90), "fx/green");
    fx.ids[kFxPlayer] = assets.add(burst_config(kYellow, 140), "fx/player");
    fx.ids[kFxUfo] = assets.add(burst_config(kRed, 130), "fx/ufo");
    fx.ids[kFxChip] = assets.add(burst_config(kShield, 50), "fx/chip");
    ctx.app().insert_resource<FxIds>(fx);
    SceneStack &stack = ctx.res<SceneStack>();
    stack.push(std::make_unique<PlayScene>());
    if (!ctx.app().options().has("--bot")) stack.push(std::make_unique<TitleScene>());
}

} // namespace

int main(int argc, char **argv) {
    Bot bot;                                                    // outlives the App
    App app({.title = "Invaders", .window_size = {960, 720}, .virtual_size = {kW, kH}, .audio = false}, argc, argv);
    app.add_plugins(DefaultPlugins{.render = {.clear_color = kBg}});
    app.add_plugin<RulesPlugin>();
    app.add_plugin<ViewPlugin>();
    app.insert_resource<Session>();
    ActionMap &map = app.res<ActionMap>();
    const auto k = [](Key key) { return ActionMap::key(key); };
    const Controls controls{
        .left = map.button("left", {k(Key::Left), k(Key::A), ActionMap::pad(PadButton::DLeft)}),
        .right = map.button("right", {k(Key::Right), k(Key::D), ActionMap::pad(PadButton::DRight)}),
        .fire = map.button("fire", {k(Key::Space), k(Key::Up), k(Key::W), ActionMap::mouse(MouseButton::Left),
                                    ActionMap::pad(PadButton::South)}),
        .confirm = map.button("confirm", {k(Key::Enter), ActionMap::pad(PadButton::Start)}),
        .pause = map.button("pause", {k(Key::P), k(Key::Escape)})};
    app.insert_resource<Controls>(controls);
    const bool bot_run = app.options().has("--bot");
    if (bot_run && app.options().replay.empty()) {
        bot.controls = controls;
        app.res<InputSystem>().set_driver(&bot);
    }
    app.add_startup(startup, "invaders.startup");
    const int code = app.run();
    app.res<InputSystem>().set_driver(nullptr);
    if (app.options().headless) {
        const Session &s = app.res<Session>();
        std::printf("invaders summary: score=%d high=%d wave=%d lives=%d kills=%d runs=%d ticks=%" PRIu64
                    " hash=%016" PRIx64 "\n",
                    s.last_score, s.high, s.last_wave, s.last_lives, s.last_kills, s.runs, app.tick(), s.last_hash);
    }
    return code;
}
