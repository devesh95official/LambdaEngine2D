// Tetris: scenes, drawing and effects. Rules are in src/tetris/.
//   tetris --bot [--bot-reckless]             watch the bot
//   tetris --headless --bot --frames 3000     no window, prints a summary line
#include <algorithm>
#include <cinttypes>
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
#include "tetris/bot.h"
#include "tetris/rules.h"

using namespace lam;
using namespace tetris;

namespace {

constexpr IVec2 kVirtual{480, 270};
constexpr Color kBg{16, 18, 30, 255};
constexpr Color kPanel{28, 31, 48, 255};
constexpr Color kGrid{38, 42, 62, 255};
constexpr Color kInk{226, 230, 240, 255};
constexpr Color kDim{130, 138, 165, 255};
constexpr Color kGold{246, 200, 82, 255};
constexpr Color kPiece[8] = {{0, 0, 0, 0},       {84, 206, 226, 255}, {242, 204, 74, 255},  {172, 104, 214, 255},
                             {112, 204, 104, 255}, {232, 84, 84, 255},  {82, 124, 232, 255}, {242, 152, 62, 255}};

// Kept across runs: best score and the headless summary numbers.
struct Session {
    int runs = 0, best = 0, last_score = 0, last_lines = 0, last_tetrises = 0, games_over = 0, total_lines = 0,
        tetrises = 0, over_wait = 0;
    std::uint64_t last_hash = 0;
};

Color scale(Color c, float k) {
    return {static_cast<std::uint8_t>(std::min(255.0f, c.r * k)), static_cast<std::uint8_t>(std::min(255.0f, c.g * k)),
            static_cast<std::uint8_t>(std::min(255.0f, c.b * k)), c.a};
}

void block(RenderQueue &q, Rect r, Color c, std::uint8_t layer) {
    q.rect(r, scale(c, 0.55f), layer);
    q.rect({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, c, static_cast<std::uint8_t>(layer + 1));
    q.rect({r.x + 2, r.y + 2, r.w - 5, 2}, scale(c, 1.35f), static_cast<std::uint8_t>(layer + 2));
}

void label(const Ctx &ctx, std::string_view text, Vec2 pos, float size, Color color, bool centred = false,
           std::uint8_t layer = kLayerUi) {
    const RenderDefaults *d = ctx.try_res<RenderDefaults>();
    const Font *font = d != nullptr ? ctx.res<AssetServer>().get(d->font) : nullptr;
    if (font == nullptr) return;
    if (centred) pos.x -= measure_text(*font, text, size).x * 0.5f;
    draw_text(ctx.res<RenderQueue>(), *font, text, pos, TextStyle{.size = size, .color = color, .layer = layer});
}

template <class... A>
void labelf(const Ctx &ctx, Vec2 pos, float size, Color color, std::format_string<A...> fmt, A &&...args) {
    char buf[64];
    const auto r = std::format_to_n(buf, sizeof buf - 1, fmt, std::forward<A>(args)...);
    label(ctx, std::string_view(buf, static_cast<std::size_t>(r.out - buf)), pos, size, color);
}

// A small piece centred in a box, for the hold and next panels.
void mini_piece(RenderQueue &q, int kind, Vec2 centre, float cell, bool dim) {
    if (kind == kNone) return;
    const auto cs = cells_of(kind, 0);
    int x0 = 9, x1 = -9, y0 = 9, y1 = -9;
    for (const IVec2 c : cs) x0 = std::min(x0, c.x), x1 = std::max(x1, c.x), y0 = std::min(y0, c.y), y1 = std::max(y1, c.y);
    const Vec2 org{centre.x - (static_cast<float>(x1 - x0 + 1) * cell) / 2, centre.y - (static_cast<float>(y1 - y0 + 1) * cell) / 2};
    for (const IVec2 c : cs)
        block(q, {org.x + static_cast<float>(c.x - x0) * cell, org.y + static_cast<float>(c.y - y0) * cell, cell, cell},
              dim ? scale(kPiece[kind], 0.45f) : kPiece[kind], kLayerUi);
}

// Board, stack, ghost, active piece and the clear flash. World layers, so they shake.
void draw_board(Ctx &ctx) {
    const Board &b = ctx.local<Board>();
    RenderQueue &q = ctx.res<RenderQueue>();
    const Rect frame{kOrigin.x - 3, kOrigin.y - 3, kW * kCell + 6, kH * kCell + 6};
    q.rect(frame, kGrid, 10);
    q.rect({kOrigin.x - 1, kOrigin.y - 1, kW * kCell + 2, kH * kCell + 2}, kPanel, 11);
    for (int x = 1; x < kW; ++x)
        q.rect({kOrigin.x + static_cast<float>(x) * kCell, kOrigin.y, 1, kH * kCell}, kGrid, 12);
    for (int y = 1; y < kH; ++y)
        q.rect({kOrigin.x, kOrigin.y + static_cast<float>(y) * kCell, kW * kCell, 1}, kGrid, 12);
    const auto cell_rect = [](int x, int y) {
        return Rect{kOrigin.x + static_cast<float>(x) * kCell, kOrigin.y + static_cast<float>(y) * kCell, kCell, kCell};
    };
    const bool flash_on = b.flash > 0 && (b.flash / 3) % 2 == 0;
    for (int y = 0; y < kH; ++y) {
        const bool clearing = (b.clear_mask >> y) & 1u;
        for (int x = 0; x < kW; ++x) {
            const std::uint8_t k = b.cells[static_cast<std::size_t>(y * kW + x)];
            if (k == 0) continue;
            block(q, cell_rect(x, y), clearing && flash_on ? Color{255, 255, 255, 255} : b.over ? scale(kPiece[k], 0.5f) : kPiece[k], 20);
        }
    }
    if (b.kind == kNone) return;
    const int ghost = b.y + drop_distance(b);
    for (const IVec2 c : cells_of(b.kind, b.rot)) {
        if (ghost + c.y >= 0) {
            Color g = kPiece[b.kind];
            g.a = 110;
            q.rect_lines(cell_rect(b.x + c.x, ghost + c.y), 1, g, 30);
        }
        if (b.y + c.y >= 0) block(q, cell_rect(b.x + c.x, b.y + c.y), kPiece[b.kind], 40);
    }
}

// Side panels: hold, next and the numbers.
void draw_hud(Ctx &ctx) {
    const Board &b = ctx.local<Board>();
    const Session &s = ctx.res<Session>();
    RenderQueue &q = ctx.res<RenderQueue>();
    q.rect({40, 18, 120, 70}, kPanel, kLayerUi);
    label(ctx, "HOLD", {50, 24}, 8, kDim);
    mini_piece(q, b.hold, {100, 60}, 10, !b.can_hold);
    q.rect({40, 98, 120, 160}, kPanel, kLayerUi);
    labelf(ctx, {50, 108}, 8, kDim, "SCORE");
    labelf(ctx, {50, 120}, 16, kInk, "{}", b.score);
    labelf(ctx, {50, 146}, 8, kDim, "BEST");
    labelf(ctx, {50, 158}, 8, kGold, "{}", std::max(s.best, b.score));
    labelf(ctx, {50, 180}, 8, kDim, "LEVEL");
    labelf(ctx, {50, 192}, 16, kInk, "{}", b.level);
    labelf(ctx, {50, 218}, 8, kDim, "LINES");
    labelf(ctx, {50, 230}, 16, kInk, "{}", b.lines);
    q.rect({320, 18, 120, 200}, kPanel, kLayerUi);
    label(ctx, "NEXT", {330, 24}, 8, kDim);
    for (int i = 0; i < 4; ++i)
        mini_piece(q, b.next[static_cast<std::size_t>(i)], {380, 56.0f + 42.0f * static_cast<float>(i)}, i == 0 ? 11.0f : 8.0f, false);
    label(ctx, "P  PAUSE", {330, 236}, 8, kDim);
}

void dim_screen(const Ctx &ctx) {
    ctx.res<RenderQueue>().rect({0, 0, static_cast<float>(kVirtual.x), static_cast<float>(kVirtual.y)}, {8, 9, 16, 190}, kLayerOverlay);
}
bool pressed(const Ctx &ctx, ButtonAction Controls::*a) { return input_frame(ctx).is_pressed(ctx.res<Controls>().*a); }

class PlayScene final : public Scene {
public:
    std::string_view name() const override { return "play"; }
    void enter(App &app, World &w) override {
        start_run(w);
        const Entity cam = w.create();
        w.add<Camera2D>(cam, Camera2D{.position = {kVirtual.x / 2.0f, kVirtual.y / 2.0f}});
        w.add<CameraShake>(cam, CameraShake{.max_offset = 6});
        ++app.res<Session>().runs;
        app.res<Session>().over_wait = 0;
    }
};

class Overlay : public Scene {
public:
    bool overlay() const override { return true; }
};

void title_tick(Ctx &ctx) {
    if (pressed(ctx, &Controls::confirm) && ctx.res<SceneStack>().queued() == 0) ctx.res<SceneStack>().pop();
}
void title_draw(Ctx &ctx) {
    dim_screen(ctx);
    const float cx = kVirtual.x / 2.0f;
    for (int i = 1; i <= 7; ++i)
        block(ctx.res<RenderQueue>(), {cx - 52 + static_cast<float>(i - 1) * 14, 62, 12, 12}, kPiece[i], kLayerOverlay);
    label(ctx, "TETRIS", {cx, 84}, 32, kInk, true, kLayerOverlay);
    label(ctx, "LEFT/RIGHT move   UP/X rotate   Z rotate back", {cx, 136}, 8, kDim, true, kLayerOverlay);
    label(ctx, "DOWN soft drop   SPACE hard drop   C hold", {cx, 150}, 8, kDim, true, kLayerOverlay);
    if (ctx.res<Session>().best > 0) {
        char buf[32];
        const auto r = std::format_to_n(buf, sizeof buf, "BEST {}", ctx.res<Session>().best);
        label(ctx, std::string_view(buf, static_cast<std::size_t>(r.out - buf)), {cx, 172}, 8, kGold, true, kLayerOverlay);
    }
    label(ctx, "ENTER or CLICK to start", {cx, 196}, 8, kGold, true, kLayerOverlay);
}
class TitleScene final : public Overlay {
public:
    std::string_view name() const override { return "title"; }
    void enter(App &app, World &) override {
        add_system(app, Stage::Simulate, title_tick, "title.tick");
        add_system(app, Stage::RenderUI, title_draw, "title.draw");
    }
};

void pause_tick(Ctx &ctx) {
    if ((pressed(ctx, &Controls::pause) || pressed(ctx, &Controls::confirm)) && ctx.res<SceneStack>().queued() == 0)
        ctx.res<SceneStack>().pop();
}
void pause_draw(Ctx &ctx) {
    dim_screen(ctx);
    label(ctx, "PAUSED", {kVirtual.x / 2.0f, 110}, 16, kInk, true, kLayerOverlay);
    label(ctx, "P or ENTER to resume", {kVirtual.x / 2.0f, 136}, 8, kDim, true, kLayerOverlay);
}
class PauseScene final : public Overlay {
public:
    std::string_view name() const override { return "pause"; }
    void enter(App &app, World &) override {
        add_system(app, Stage::Simulate, pause_tick, "pause.tick");
        add_system(app, Stage::RenderUI, pause_draw, "pause.draw");
    }
};

void over_tick(Ctx &ctx) {
    if (pressed(ctx, &Controls::confirm) && ctx.res<SceneStack>().queued() == 0)
        ctx.res<SceneStack>().reset(std::make_unique<PlayScene>());
}
void over_draw(Ctx &ctx) {
    const Session &s = ctx.res<Session>();
    const float cx = kVirtual.x / 2.0f;
    dim_screen(ctx);
    label(ctx, "GAME OVER", {cx, 96}, 16, {240, 96, 96, 255}, true, kLayerOverlay);
    char buf[48];
    auto r = std::format_to_n(buf, sizeof buf, "SCORE {}", s.last_score);
    label(ctx, std::string_view(buf, static_cast<std::size_t>(r.out - buf)), {cx, 124}, 8, kInk, true, kLayerOverlay);
    r = std::format_to_n(buf, sizeof buf, "BEST {}", s.best);
    label(ctx, std::string_view(buf, static_cast<std::size_t>(r.out - buf)), {cx, 138}, 8, kGold, true, kLayerOverlay);
    label(ctx, "ENTER or CLICK to play again", {cx, 162}, 8, kDim, true, kLayerOverlay);
}
class OverScene final : public Overlay {
public:
    std::string_view name() const override { return "over"; }
    void enter(App &app, World &) override {
        add_system(app, Stage::Simulate, over_tick, "over.tick");
        add_system(app, Stage::RenderUI, over_draw, "over.draw");
    }
};

// After the rules: track the best score, handle pause, push OverScene shortly after game over.
void play_flow(Ctx &ctx) {
    const Board &b = ctx.local<Board>();
    Session &s = ctx.res<Session>();
    SceneStack &stack = ctx.res<SceneStack>();
    s.best = std::max(s.best, b.score);
    s.last_score = b.score;
    s.last_lines = b.lines;
    s.last_tetrises = b.tetrises;
    if (stack.queued() > 0) return;
    if (b.over) {
        if (++s.over_wait == 50) {
            ++s.games_over;
            s.total_lines += b.lines;
            s.tetrises += b.tetrises;
            stack.push(std::make_unique<OverScene>());
        }
        return;
    }
    if (pressed(ctx, &Controls::pause)) stack.push(std::make_unique<PauseScene>());
}
void record_hash(Ctx &ctx) { ctx.res<Session>().last_hash = ctx.world().state_hash(); }

EmitterConfig burst_config(Color a, Color b, float speed, float size) {
    EmitterConfig c;
    c.capacity = 512;
    c.speed_min = speed * 0.3f;
    c.speed_max = speed;
    c.life_min = 0.25f;
    c.life_max = 0.6f;
    c.size_start = size;
    c.color_start = a;
    c.color_end = b;
    c.gravity = {0, 220};
    c.drag = 1.2f;
    return sanitize(c);
}

void load_game(Ctx &ctx) {
    AssetServer &assets = ctx.res<AssetServer>();
    ctx.app().insert_resource<Fx>(Fx{
        .spark = assets.add(burst_config({255, 244, 200, 255}, {246, 200, 82, 0}, 150, 3), "fx/spark"),
        .dust = assets.add(burst_config({170, 178, 205, 255}, {90, 96, 120, 0}, 60, 2), "fx/dust")});
    SceneStack &stack = ctx.res<SceneStack>();
    stack.push(std::make_unique<PlayScene>());
    if (!ctx.app().options().has("--bot")) stack.push(std::make_unique<TitleScene>());
}

} // namespace

int main(int argc, char **argv) {
    std::unique_ptr<Bot> bot;                                   // outlives the App
    App app({.title = "Tetris", .virtual_size = kVirtual, .audio = false}, argc, argv);
    app.add_plugins(DefaultPlugins{.render = {.clear_color = kBg}});
    app.add_plugin<RulesPlugin>();
    app.insert_resource<Session>();
    ActionMap &map = app.res<ActionMap>();
    const auto k = [](Key key) { return ActionMap::key(key); };
    const Controls c{.left = map.button("left", {k(Key::Left), k(Key::A)}),
                     .right = map.button("right", {k(Key::Right), k(Key::D)}),
                     .soft = map.button("soft_drop", {k(Key::Down), k(Key::S)}),
                     .drop = map.button("hard_drop", {k(Key::Space)}),
                     .rot_cw = map.button("rotate_cw", {k(Key::Up), k(Key::X), k(Key::W)}),
                     .rot_ccw = map.button("rotate_ccw", {k(Key::Z), k(Key::LCtrl)}),
                     .hold = map.button("hold", {k(Key::C), k(Key::LShift)}),
                     .confirm = map.button("confirm", {k(Key::Enter), ActionMap::mouse(MouseButton::Left)}),
                     .pause = map.button("pause", {k(Key::P), k(Key::Escape)})};
    map.set_repeat(c.left, 10, 3);
    map.set_repeat(c.right, 10, 3);
    app.insert_resource<Controls>(c);
    app.add_system({.stage = Stage::Simulate, .fn = play_flow, .name = "tetris.flow", .order = 100, .run_if = has_local<Board>});
    app.add_system({.stage = Stage::TickEnd, .fn = record_hash, .name = "tetris.hash", .order = 900, .run_if = has_local<Board>});
    app.add_system({.stage = Stage::Render, .fn = draw_board, .name = "tetris.board", .run_if = has_local<Board>});
    app.add_system({.stage = Stage::RenderUI, .fn = draw_hud, .name = "tetris.hud", .run_if = has_local<Board>});
    app.add_startup(load_game, "tetris.load");
    const bool bot_run = app.options().has("--bot");
    if (bot_run && app.options().replay.empty()) {
        bot = std::make_unique<Bot>(c, app.options().has("--bot-reckless"));
        app.res<InputSystem>().set_driver(bot.get());
    }
    const int code = app.run();
    if (bot) app.res<InputSystem>().set_driver(nullptr);
    if (app.options().headless && bot_run) {
        const Session &s = app.res<Session>();
        std::printf("tetris summary: score=%d runs=%d games_over=%d lines=%d tetrises=%d last_score=%d ticks=%" PRIu64
                    " hash=%016" PRIx64 "\n",
                    s.best, s.runs, s.games_over, s.total_lines + (s.over_wait < 50 ? s.last_lines : 0),
                    s.tetrises + (s.over_wait < 50 ? s.last_tetrises : 0), s.last_score, app.tick(), s.last_hash);
    }
    return code;
}
