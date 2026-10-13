// 2048: drawing and effects. Rules are in src/g2048/.
//   g2048 --bot                               watch the bot
//   g2048 --headless --bot --frames 3000      no window, prints a summary line
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
#include <lam/ecs/world.h>
#include <lam/input/input_plugin.h>
#include <lam/math/ease.h>
#include <lam/particles/particles.h>
#include <lam/render/camera.h>
#include <lam/render/font.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include "g2048/rules.h"

using namespace lam;
using namespace g2048;

namespace {

constexpr Color kBg{250, 248, 239, 255};
constexpr Color kBoard{187, 173, 160, 255};
constexpr Color kEmpty{205, 193, 180, 255};
constexpr Color kInk{119, 110, 101, 255};
constexpr Color kLight{249, 246, 242, 255};
constexpr Color kAccent{143, 122, 102, 255};
constexpr std::uint8_t kLayerBoard = 20, kLayerTile = 30, kLayerTileText = 34;
constexpr std::uint8_t kLayerHud = kLayerUi, kLayerHudText = kLayerUi + 1;
constexpr std::uint8_t kLayerOver = kLayerUi + 4, kLayerOverText = kLayerUi + 5;

Color tile_color(int v) {
    switch (v) {
    case 2: return {238, 228, 218, 255};
    case 4: return {237, 224, 200, 255};
    case 8: return {242, 177, 121, 255};
    case 16: return {245, 149, 99, 255};
    case 32: return {246, 124, 95, 255};
    case 64: return {246, 94, 59, 255};
    case 128: return {237, 207, 114, 255};
    case 256: return {237, 204, 97, 255};
    case 512: return {237, 200, 80, 255};
    case 1024: return {237, 197, 63, 255};
    case 2048: return {237, 194, 46, 255};
    default: return {60, 58, 50, 255};
    }
}

void rounded(RenderQueue &q, Rect r, float rad, Color c, std::uint8_t layer, float depth = 0) {
    rad = std::min(rad, std::min(r.w, r.h) * 0.5f);
    q.rect({r.x + rad, r.y, r.w - 2 * rad, r.h}, c, layer, depth);
    q.rect({r.x, r.y + rad, rad, r.h - 2 * rad}, c, layer, depth);
    q.rect({r.x + r.w - rad, r.y + rad, rad, r.h - 2 * rad}, c, layer, depth);
    for (Vec2 k : {Vec2{r.x + rad, r.y + rad}, Vec2{r.x + r.w - rad, r.y + rad}, Vec2{r.x + rad, r.y + r.h - rad},
                   Vec2{r.x + r.w - rad, r.y + r.h - rad}})
        q.circle(k, rad, c, layer, depth);
}

// Text centred on `at` (both axes).
void label(const Ctx &ctx, std::string_view s, Vec2 at, float size, Color col, std::uint8_t layer, float depth = 0) {
    const RenderDefaults *d = ctx.try_res<RenderDefaults>();
    const Font *font = d != nullptr ? ctx.res<AssetServer>().get(d->font) : nullptr;
    if (font == nullptr) return;
    const Vec2 m = measure_text(*font, s, size);
    draw_text(ctx.res<RenderQueue>(), *font, s, {std::round(at.x - m.x * 0.5f), std::round(at.y - m.y * 0.5f)},
              TextStyle{.size = size, .color = col, .layer = layer}, depth);
}

void draw_tile(const Ctx &ctx, Vec2 centre, int v, float scale, float depth) {
    const float s = kTile * scale;
    rounded(ctx.res<RenderQueue>(), {centre.x - s / 2, centre.y - s / 2, s, s}, 4 * scale, tile_color(v), kLayerTile, depth);
    if (scale < 0.5f) return;
    char buf[12];
    const auto r = std::format_to_n(buf, sizeof buf, "{}", v);
    const auto n = static_cast<std::size_t>(r.out - buf);
    const float size = n <= 2 ? 24.0f : n <= 4 ? 16.0f : 8.0f;
    label(ctx, std::string_view(buf, n), {centre.x + 1, centre.y + 1}, size, v <= 4 ? kInk : kLight, kLayerTileText, depth);
}

// Board and tiles on world layers so the camera shake moves them.
void draw_board(Ctx &ctx) {
    const Game &g = ctx.local<Game>();
    RenderQueue &q = ctx.res<RenderQueue>();
    q.rect({-40, -40, kVirtual.x + 80, kVirtual.y + 80}, kBg, kLayerBackground);
    rounded(q, {kBoardPos.x, kBoardPos.y, kBoardSize, kBoardSize}, 6, kBoard, kLayerBoard, 0);
    for (int i = 0; i < kCells; ++i) {
        const Vec2 p = cell_pos(i);
        rounded(q, {p.x, p.y, kTile, kTile}, 4, kEmpty, kLayerBoard, 1);
    }
    const float ticks = static_cast<float>(g.since_move) + ctx.time().alpha;
    const float t = ease(Ease::OutCubic, ticks / kSlideTicks);
    const float pop = std::clamp((ticks - kSlideTicks) / kPopTicks, 0.0f, 1.0f);
    const bool sliding = ticks < kSlideTicks;
    float depth = 0;
    for (int i = 0; i < kCells; ++i) {
        const int v = g.cells[i];
        if (v == 0) continue;
        const bool fresh = i == g.spawned || g.src_a[i] == -2;
        if (sliding && g.src_a[i] >= 0) {
            const Vec2 to = cell_center(i);
            const int part = g.src_b[i] >= 0 ? v / 2 : v;
            for (int src : {g.src_a[i], g.src_b[i]}) {
                if (src < 0) continue;
                const Vec2 from = cell_center(src);
                draw_tile(ctx, from + (to - from) * t, part, 1, depth++);
            }
            continue;
        }
        if (sliding && fresh) continue;                         // shows up after the slide
        float scale = 1;
        if (g.src_b[i] >= 0 && pop < 1) scale = 1 + 0.18f * std::sin(pop * 3.14159f);
        if (fresh && pop < 1) scale = std::max(0.05f, ease(Ease::OutBack, pop));
        draw_tile(ctx, cell_center(i), v, scale, 100 + depth++);
    }
}

void button(const Ctx &ctx, Rect r, std::string_view text, bool enabled) {
    rounded(ctx.res<RenderQueue>(), r, 3, enabled ? kAccent : kEmpty, kLayerHud);
    label(ctx, text, {r.x + r.w / 2, r.y + r.h / 2 + 1}, 8, enabled ? kLight : kBoard, kLayerHudText);
}

// Side panel and overlays.
void draw_hud(Ctx &ctx) {
    const Game &g = ctx.local<Game>();
    RenderQueue &q = ctx.res<RenderQueue>();
    const float px = 380;                                       // panel centre
    char buf[32];
    const auto fmt = [&](std::string_view prefix, int v) {     // no allocation
        const auto r = std::format_to_n(buf, sizeof buf, "{}{}", prefix, v);
        return std::string_view(buf, static_cast<std::size_t>(r.out - buf));
    };
    label(ctx, "2048", {px, 36}, 40, kInk, kLayerHudText);
    for (int k = 0; k < 2; ++k) {
        const Rect box{300.0f + static_cast<float>(k) * 84, 66, 76, 40};
        rounded(q, box, 4, kBoard, kLayerHud);
        label(ctx, k == 0 ? "SCORE" : "BEST", {box.x + box.w / 2, box.y + 10}, 8, {238, 228, 218, 255}, kLayerHudText);
        label(ctx, fmt("", k == 0 ? g.score : g.best), {box.x + box.w / 2, box.y + 27}, 16, kLight, kLayerHudText);
    }
    label(ctx, fmt("MOVES ", g.moves), {px - 40, 118}, 8, kInk, kLayerHudText);
    label(ctx, fmt("BIGGEST ", max_tile(g.cells)), {px + 40, 118}, 8, kInk, kLayerHudText);
    label(ctx, "ARROWS / WASD  SLIDE", {px, 140}, 8, kAccent, kLayerHudText);
    label(ctx, "Z  UNDO    R  NEW GAME", {px, 154}, 8, kAccent, kLayerHudText);
    label(ctx, "MOUSE  SWIPE OR CLICK", {px, 168}, 8, kAccent, kLayerHudText);
    button(ctx, kNewButton, "NEW GAME", true);
    button(ctx, kUndoButton, "UNDO", g.can_undo);
    label(ctx, "JOIN THE TILES,", {px, 230}, 8, kInk, kLayerHudText);
    label(ctx, "GET TO THE 2048 TILE!", {px, 242}, 8, kInk, kLayerHudText);

    const bool blink = std::fmod(ctx.time().real_seconds, 1.0) < 0.65;
    const Vec2 bc{kBoardPos.x + kBoardSize / 2, kBoardPos.y + kBoardSize / 2};
    const Rect board{kBoardPos.x, kBoardPos.y, kBoardSize, kBoardSize};
    switch (g.phase) {
    case Phase::Title:
        q.rect({0, 0, kVirtual.x, kVirtual.y}, kBg, kLayerOver);
        for (int i = 0; i < 4; ++i)                             // a row of sample tiles
            rounded(q, {140.0f + static_cast<float>(i) * 52, 34, 44, 44}, 4, tile_color(2 << (i * 3)), kLayerOver, 1);
        label(ctx, "2048", {240, 120}, 64, kInk, kLayerOverText);
        label(ctx, "SLIDE THE TILES. MATCHING NUMBERS MERGE.", {240, 162}, 8, kAccent, kLayerOverText);
        label(ctx, "REACH 2048 TO WIN.", {240, 176}, 8, kAccent, kLayerOverText);
        if (blink) label(ctx, "PRESS ENTER OR CLICK TO PLAY", {240, 206}, 16, kInk, kLayerOverText);
        label(ctx, "ARROWS/WASD SLIDE   Z UNDO   R NEW GAME", {240, 240}, 8, kAccent, kLayerOverText);
        if (g.best > 0) label(ctx, fmt("BEST ", g.best), {240, 254}, 8, kInk, kLayerOverText);
        break;
    case Phase::Won:
        rounded(q, board, 6, {237, 194, 46, 190}, kLayerOver);
        label(ctx, "YOU WIN!", {bc.x, bc.y - 30}, 32, kLight, kLayerOverText);
        label(ctx, fmt("SCORE ", g.score), {bc.x, bc.y + 4}, 16, kLight, kLayerOverText);
        label(ctx, "ENTER  KEEP GOING", {bc.x, bc.y + 34}, 8, kLight, kLayerOverText);
        label(ctx, "R  NEW GAME", {bc.x, bc.y + 48}, 8, kLight, kLayerOverText);
        break;
    case Phase::Over:
        q.rect(board, {238, 228, 218, 200}, kLayerOver);
        label(ctx, "GAME OVER", {bc.x, bc.y - 30}, 24, kInk, kLayerOverText);
        label(ctx, fmt("SCORE ", g.score), {bc.x, bc.y + 2}, 16, kInk, kLayerOverText);
        if (g.score >= g.best && g.score > 0) label(ctx, "NEW BEST!", {bc.x, bc.y + 20}, 8, {246, 94, 59, 255}, kLayerOverText);
        if (blink) label(ctx, "ENTER  NEW GAME", {bc.x, bc.y + 40}, 8, kInk, kLayerOverText);
        label(ctx, "Z  UNDO LAST MOVE", {bc.x, bc.y + 54}, 8, kAccent, kLayerOverText);
        break;
    case Phase::Play: break;
    }
}

void start(Ctx &ctx) {
    World &w = ctx.world();
    init_game(w);
    const Entity cam = w.create();                              // only here for the shake
    w.add<Camera2D>(cam, Camera2D{.position = {kVirtual.x / 2, kVirtual.y / 2}});
    w.add<CameraShake>(cam, CameraShake{.max_offset = 5});
}

void load_fx(App &app) {
    AssetServer &assets = app.res<AssetServer>();
    EmitterConfig pop;
    pop.capacity = 512;
    pop.speed_min = 40;
    pop.speed_max = 120;
    pop.life_min = 0.25f;
    pop.life_max = 0.55f;
    pop.size_start = 3.5f;
    pop.size_end = 0;
    pop.drag = 2;
    pop.color_start = Color{246, 124, 95, 255};
    pop.color_end = Color{237, 194, 46, 0};
    pop.layer = kLayerTile + 2;                                // over tiles, under digits
    EmitterConfig confetti = pop;
    confetti.capacity = 1024;
    confetti.speed_min = 60;
    confetti.speed_max = 200;
    confetti.life_max = 1.1f;
    confetti.gravity = {0, 160};
    confetti.color_start = Color{237, 194, 46, 255};
    confetti.color_end = Color{246, 94, 59, 0};
    app.insert_resource<Fx>(Fx{.pop = assets.add(sanitize(pop), "fx/pop"), .confetti = assets.add(sanitize(confetti), "fx/confetti")});
}

}  // namespace

int main(int argc, char **argv) {
    std::unique_ptr<Bot> bot;                                   // outlives the App
    App app({.title = "2048", .window_size = {1280, 720}, .virtual_size = {480, 270}}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    app.add_plugin<RulesPlugin>();
    load_fx(app);
    app.add_startup(start, "g2048.start");
    app.add_system({.stage = Stage::Render, .fn = draw_board, .name = "g2048.board", .run_if = &has_local<Game>});
    app.add_system({.stage = Stage::RenderUI, .fn = draw_hud, .name = "g2048.hud", .run_if = &has_local<Game>});
    if (app.options().has("--bot")) {
        bot = std::make_unique<Bot>(app.res<Controls>());
        app.res<InputSystem>().set_driver(bot.get());
    }
    const int code = app.run();
    if (app.options().headless) {
        const Game &g = app.main_world().resources().get<Game>();
        std::printf("g2048 summary: score=%d best=%d moves=%d max_tile=%d games=%d ticks=%" PRIu64 " hash=%016" PRIx64 "\n",
                    g.score, g.best, g.moves, max_tile(g.cells), g.games, app.tick(), app.main_world().state_hash());
    }
    app.res<InputSystem>().set_driver(nullptr);
    return code;
}
