// Minesweeper: board, HUD, screens and effects. Rules are in src/minesweeper/.
//   minesweeper --bot                            watch the bot
//   minesweeper --headless --bot --frames 3000   no window, prints a summary line
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <utility>
#include <cstdio>
#include <format>
#include <string_view>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/keys.h>
#include <lam/render/camera.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/render/shapes.h>
#include "minesweeper/rules.h"

using namespace lam;
using namespace ms;

namespace {

constexpr Color kBg{22, 24, 36, 255}, kPanel{34, 38, 56, 255}, kGrid{16, 18, 28, 255};
constexpr Color kHidden{84, 96, 132, 255}, kHiddenHi{112, 126, 168, 255}, kHiddenLo{58, 66, 94, 255};
constexpr Color kOpenC{44, 50, 70, 255}, kInk{232, 230, 220, 255}, kDim{140, 146, 170, 255};
constexpr Color kAccent{255, 204, 92, 255}, kRed{236, 84, 84, 255}, kGreen{110, 214, 140, 255};
constexpr Color kNum[9] = {{0, 0, 0, 0},         {110, 170, 255, 255}, {110, 214, 140, 255},
                           {255, 120, 110, 255}, {190, 140, 255, 255}, {255, 180, 90, 255},
                           {90, 220, 220, 255},  {240, 240, 240, 255}, {160, 160, 176, 255}};
constexpr std::uint8_t kLayerCells = kLayerTiles, kLayerMarks = kLayerTiles + 1, kLayerHover = kLayerTiles + 2;

void text(const Ctx &ctx, std::string_view s, Vec2 pos, float size, Color c, std::uint8_t layer = kLayerUi,
          bool centred = true) {
    const RenderDefaults *d = ctx.try_res<RenderDefaults>();
    const Font *font = d != nullptr ? ctx.res<AssetServer>().get(d->font) : nullptr;
    if (font == nullptr) return;
    if (centred) pos.x -= std::floor(measure_text(*font, s, size).x * 0.5f);
    draw_text(ctx.res<RenderQueue>(), *font, s, pos, TextStyle{.size = size, .color = c, .layer = layer});
}

template <class... A> std::string_view fmt(char (&buf)[64], std::format_string<A...> f, A &&...a) {
    const auto r = std::format_to_n(buf, sizeof buf - 1, f, std::forward<A>(a)...);
    return {buf, static_cast<std::size_t>(r.out - buf)};
}

void bevel(RenderQueue &q, Rect r, std::uint8_t layer) {
    q.rect(r, kHiddenLo, layer);
    q.rect({r.x, r.y, r.w - 2, r.h - 2}, kHiddenHi, layer);
    q.rect({r.x + 2, r.y + 2, r.w - 4, r.h - 4}, kHidden, layer);
}

void draw_flag(RenderQueue &q, Vec2 c, float s) {
    q.rect({c.x - s * 0.05f, c.y - s * 0.3f, std::max(1.0f, s * 0.08f), s * 0.6f}, kInk, kLayerMarks);
    q.rect({c.x - s * 0.22f, c.y + s * 0.25f, s * 0.44f, std::max(1.0f, s * 0.08f)}, kInk, kLayerMarks);
    draw_triangle(q, {c.x, c.y - s * 0.32f}, {c.x, c.y + s * 0.02f}, {c.x - s * 0.32f, c.y - s * 0.15f}, kRed,
                  kLayerMarks);
}

void draw_mine(RenderQueue &q, Vec2 c, float s, std::uint8_t layer = kLayerMarks) {
    const float r = s * 0.24f;
    q.line({c.x - r * 1.5f, c.y}, {c.x + r * 1.5f, c.y}, 1, kGrid, layer);
    q.line({c.x, c.y - r * 1.5f}, {c.x, c.y + r * 1.5f}, 1, kGrid, layer);
    q.circle(c, r, kGrid, layer);
    q.rect({c.x - r * 0.5f, c.y - r * 0.5f, std::max(1.0f, r * 0.4f), std::max(1.0f, r * 0.4f)}, kInk, layer);
}

// Board on world layers so the camera shake moves it.
void draw_board(Ctx &ctx) {
    const Mine &m = ctx.local<Mine>();
    if (m.phase == kTitle) return;
    RenderQueue &q = ctx.res<RenderQueue>();
    const Rect b = board_rect(m);
    const float s = b.w / static_cast<float>(m.w);
    q.rect({b.x - 4, b.y - 4, b.w + 8, b.h + 8}, kPanel, kLayerCells);
    q.rect(b, kGrid, kLayerCells);
    const bool over = m.phase == kOver;
    char buf[64];
    for (int i = 0; i < m.w * m.h; ++i) {
        const std::uint8_t c = m.cells[static_cast<std::size_t>(i)];
        const Rect r{b.x + static_cast<float>(i % m.w) * s, b.y + static_cast<float>(i / m.w) * s, s, s};
        const Vec2 mid{r.x + s / 2, r.y + s / 2};
        if (c & kOpen) {
            q.rect({r.x + 1, r.y + 1, s - 1, s - 1}, i == m.boom ? kRed : kOpenC, kLayerCells);
            if (c & kMine) draw_mine(q, mid, s);
            else if (const int n = c & kCount; n > 0) {
                const float size = s >= 24 ? 16 : 8;
                text(ctx, fmt(buf, "{}", n), {mid.x, mid.y - size / 2}, size, kNum[n], kLayerMarks);
            }
            continue;
        }
        bevel(q, {r.x + 1, r.y + 1, s - 1, s - 1}, kLayerCells);
        if (over && (c & kMine) && !(c & kFlag)) draw_mine(q, mid, s);
        if (c & kFlag) {
            draw_flag(q, mid, s);
            if (over && !(c & kMine)) {                          // wrong flag
                q.line({r.x + 3, r.y + 3}, {r.x + s - 3, r.y + s - 3}, 2, kRed, kLayerHover);
                q.line({r.x + s - 3, r.y + 3}, {r.x + 3, r.y + s - 3}, 2, kRed, kLayerHover);
            }
        }
    }
    if (m.phase != kPlay) return;
    const Rect cur{b.x + static_cast<float>(m.cx) * s, b.y + static_cast<float>(m.cy) * s, s + 1, s + 1};
    q.rect_lines(cur, 1, kDim, kLayerHover);
    if (const FrameInput *fi = ctx.try_res<FrameInput>(); fi != nullptr && fi->pointer.valid)
        if (const int h = cell_at(m, fi->pointer.virtual_pos); h >= 0)
            q.rect_lines({b.x + static_cast<float>(h % m.w) * s, b.y + static_cast<float>(h / m.w) * s, s + 1, s + 1}, 2,
                         kAccent, kLayerHover);
}

void draw_title(const Ctx &ctx, const Mine &m) {
    RenderQueue &q = ctx.res<RenderQueue>();
    char buf[64];
    const float cx = static_cast<float>(kVirtual.x) / 2;
    for (int i = 0; i < 9; ++i) {                               // a strip of tiles as a banner
        const Rect r{cx - 108 + static_cast<float>(i) * 24, 26, 23, 23};
        if (i == 4) { q.rect(r, kRed, kLayerUi); continue; }
        bevel(q, r, kLayerUi);
    }
    draw_mine(q, {cx, 37.5f}, 23, kLayerUi);
    text(ctx, "MINESWEEPER", {cx, 64}, 24, kInk);
    text(ctx, "Open every safe cell. Numbers count adjacent mines.", {cx, 100}, 8, kDim);
    text(ctx, "Pick a field:", {cx, 122}, 8, kDim);
    for (int i = 0; i < 2; ++i) {
        const Rect r = title_button(i);
        const Level &lv = kLevels[static_cast<std::size_t>(i)];
        q.rect(r, i == m.level ? kHiddenLo : kPanel, kLayerUi);
        q.rect_lines(r, 2, i == m.level ? kAccent : kHiddenLo, kLayerUi);
        text(ctx, lv.name, {r.x + r.w / 2, r.y + 14}, 8, i == m.level ? kAccent : kInk);
        text(ctx, fmt(buf, "{} x {}", lv.w, lv.h), {r.x + r.w / 2, r.y + 30}, 8, kInk);
        text(ctx, fmt(buf, "{} mines", lv.mines), {r.x + r.w / 2, r.y + 44}, 8, kDim);
    }
    text(ctx, fmt(buf, "BEST {}   GAMES {}   WINS {}", m.best, m.games, m.wins), {cx, 218}, 8, kAccent);
    text(ctx, "Click a field, or 1/2 + ENTER", {cx, 240}, 8, kInk);
    text(ctx, "LMB reveal  RMB flag  MMB chord  (click a number = chord)", {cx, 256}, 8, kDim);
    text(ctx, "Keys: ARROWS move  SPACE reveal  F flag  C chord  R retry", {cx, 270}, 8, kDim);
}

// HUD, title and game-over panel.
void draw_ui(Ctx &ctx) {
    const Mine &m = ctx.local<Mine>();
    RenderQueue &q = ctx.res<RenderQueue>();
    if (m.phase == kTitle) { draw_title(ctx, m); return; }
    const float vw = static_cast<float>(kVirtual.x);
    char buf[64];
    q.rect({0, 0, vw, kHudH - 6}, kPanel, kLayerUi);
    text(ctx, fmt(buf, "{:03}", m.mines - m.flags), {10, 6}, 16, kRed, kLayerUi, false);
    text(ctx, "MINES", {58, 10}, 8, kDim, kLayerUi, false);
    text(ctx, fmt(buf, "SCORE {}", m.score), {vw / 2, 4}, 8, kInk);
    text(ctx, fmt(buf, "BEST {}", std::max(m.best, m.score)), {vw / 2, 15}, 8, kAccent);
    text(ctx, "TIME", {vw - 92, 10}, 8, kDim, kLayerUi, false);
    text(ctx, fmt(buf, "{:03}", std::min(999, seconds(m))), {vw - 10 - 36, 6}, 16, kInk, kLayerUi, false);
    if (m.phase != kOver || m.phase_ticks < 40) return;
    const Rect p{vw / 2 - 110, 120, 220, 72};
    q.rect(p, Color{22, 24, 36, 235}, kLayerOverlay);
    q.rect_lines(p, 2, m.won ? kGreen : kRed, kLayerOverlay);
    text(ctx, m.won ? "FIELD CLEARED!" : "BOOM!", {vw / 2, p.y + 10}, 16, m.won ? kGreen : kRed, kLayerOverlay);
    text(ctx, fmt(buf, "SCORE {}   BEST {}   {}s", m.score, m.best, seconds(m)), {vw / 2, p.y + 34}, 8, kInk,
         kLayerOverlay);
    text(ctx, "CLICK/ENTER: menu    R: retry", {vw / 2, p.y + 52}, 8, kDim, kLayerOverlay);
}

EmitterConfig fx(Color a, Color b, float speed, float size, float gravity) {
    EmitterConfig c;
    c.capacity = 512;
    c.speed_min = speed * 0.3f;
    c.speed_max = speed;
    c.life_min = 0.3f;
    c.life_max = 0.8f;
    c.size_start = size;
    c.color_start = a;
    c.color_end = b;
    c.gravity = {0, gravity};
    c.drag = 1.2f;
    return sanitize(c);
}

void setup(Ctx &ctx) {
    AssetServer &assets = ctx.res<AssetServer>();
    ctx.app().insert_resource<Fx>(Fx{.spark = assets.add(fx({150, 200, 255, 255}, {110, 170, 255, 0}, 70, 2, 40), "fx/spark"),
                                     .boom = assets.add(fx({255, 220, 120, 255}, {236, 60, 60, 0}, 170, 3, 120), "fx/boom"),
                                     .confetti = assets.add(fx(kAccent, {110, 214, 140, 0}, 140, 3, 160), "fx/confetti")});
    World &w = ctx.world();
    start_world(w);
    const Entity cam = w.create();
    w.add<Camera2D>(cam, Camera2D{.position = {static_cast<float>(kVirtual.x) / 2, static_cast<float>(kVirtual.y) / 2}});
    w.add<CameraShake>(cam, CameraShake{.max_offset = 5});
}

} // namespace

int main(int argc, char **argv) {
    Bot bot;                                                    // outlives the App
    App app({.title = "Minesweeper", .window_size = {1200, 900}, .virtual_size = kVirtual, .audio = false}, argc, argv);
    app.add_plugins(DefaultPlugins{.render = {.clear_color = kBg}});
    app.add_plugin<RulesPlugin>();
    ActionMap &map = app.res<ActionMap>();
    const auto key = [](Key k) { return ActionMap::key(k); };
    const Controls controls{
        .reveal = map.button("reveal", {ActionMap::mouse(MouseButton::Left), key(Key::Space), key(Key::Z)}),
        .flag = map.button("flag", {ActionMap::mouse(MouseButton::Right), key(Key::F), key(Key::X)}),
        .chord = map.button("chord", {ActionMap::mouse(MouseButton::Middle), key(Key::C)}),
        .up = map.button("up", {key(Key::Up), key(Key::W)}),
        .down = map.button("down", {key(Key::Down), key(Key::S)}),
        .left = map.button("left", {key(Key::Left), key(Key::A)}),
        .right = map.button("right", {key(Key::Right), key(Key::D)}),
        .confirm = map.button("confirm", {key(Key::Enter)}),
        .restart = map.button("restart", {key(Key::R)}),
        .back = map.button("back", {key(Key::Escape)}),
        .one = map.button("one", {key(Key::Num1)}),
        .two = map.button("two", {key(Key::Num2)})};
    app.insert_resource<Controls>(controls);
    app.add_startup(setup, "minesweeper.setup");
    app.add_system({.stage = Stage::Render, .fn = draw_board, .name = "minesweeper.board", .run_if = &has_local<Mine>});
    app.add_system({.stage = Stage::RenderUI, .fn = draw_ui, .name = "minesweeper.ui", .run_if = &has_local<Mine>});
    const bool bot_run = app.options().has("--bot");
    if (bot_run) {
        bot.c = controls;
        app.res<InputSystem>().set_driver(&bot);
    }
    const int code = app.run();
    if (bot_run) app.res<InputSystem>().set_driver(nullptr);
    if (app.options().headless) {
        const Mine *m = app.main_world().resources().try_get<Mine>();
        if (m != nullptr)
            std::printf("minesweeper summary: score=%d current=%d games=%d wins=%d losses=%d ticks=%" PRIu64
                        " hash=%016" PRIx64 "\n",
                        m->best, m->score, m->games, m->wins, m->games - m->wins, app.tick(),
                        app.main_world().state_hash());
    }
    return code;
}
