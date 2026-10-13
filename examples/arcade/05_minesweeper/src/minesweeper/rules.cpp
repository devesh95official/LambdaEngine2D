#include "minesweeper/rules.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <lam/core/id.h>
#include <lam/ecs/world.h>
#include <lam/particles/components.h>
#include <lam/render/camera_math.h>

namespace ms {
using namespace lam;
using namespace lam::literals;

Rect board_rect(const Mine &m) {
    const float cell = static_cast<float>(kLevels[static_cast<std::size_t>(m.level)].cell);
    const float bw = cell * static_cast<float>(m.w), bh = cell * static_cast<float>(m.h);
    const float vx = static_cast<float>(kVirtual.x), vy = static_cast<float>(kVirtual.y);
    return {std::floor((vx - bw) / 2), kHudH + std::floor((vy - kHudH - bh) / 2), bw, bh};
}
Rect title_button(int level) { return {static_cast<float>(30 + level * 180), 140, 160, 64}; }
Vec2 cell_center(const Mine &m, int cell) {
    const Rect r = board_rect(m);
    const float s = r.w / static_cast<float>(m.w);
    return {r.x + (static_cast<float>(cell % m.w) + 0.5f) * s, r.y + (static_cast<float>(cell / m.w) + 0.5f) * s};
}
int cell_at(const Mine &m, Vec2 p) {
    const Rect r = board_rect(m);
    if (!r.contains(p)) return -1;
    const float s = r.w / static_cast<float>(m.w);
    const int x = std::clamp(static_cast<int>((p.x - r.x) / s), 0, m.w - 1);
    const int y = std::clamp(static_cast<int>((p.y - r.y) / s), 0, m.h - 1);
    return y * m.w + x;
}

namespace {

template <class F> void each_neighbour(const Mine &m, int cell, F &&f) {
    const int x = cell % m.w, y = cell / m.w;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if ((dx != 0 || dy != 0) && nx >= 0 && ny >= 0 && nx < m.w && ny < m.h) f(ny * m.w + nx);
        }
}

void set_phase(Mine &m, std::int32_t phase) { m.phase = phase; m.phase_ticks = 0; }

void new_board(Mine &m) {
    const Level &lv = kLevels[static_cast<std::size_t>(m.level)];
    m.w = lv.w; m.h = lv.h; m.mines = lv.mines;
    m.cells.fill(0);
    m.placed = m.won = false;
    m.opened = m.flags = m.ticks = m.score = 0;
    m.cx = m.w / 2; m.cy = m.h / 2; m.boom = -1;
    set_phase(m, kPlay);
}

// Mines go down on the first click, away from the clicked cell and its neighbours.
void place_mines(Mine &m, Rng &rng, int safe) {
    const int sx = safe % m.w, sy = safe / m.w, n = m.w * m.h;
    for (int left = m.mines; left > 0;) {
        const int c = rng.between(0, n - 1);
        if ((m.cells[static_cast<std::size_t>(c)] & kMine) || (std::abs(c % m.w - sx) <= 1 && std::abs(c / m.w - sy) <= 1))
            continue;
        m.cells[static_cast<std::size_t>(c)] |= kMine;
        --left;
    }
    for (int c = 0; c < n; ++c) {
        std::uint8_t count = 0;
        each_neighbour(m, c, [&](int o) { count += (m.cells[static_cast<std::size_t>(o)] & kMine) ? 1 : 0; });
        m.cells[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(m.cells[static_cast<std::size_t>(c)] | count);
    }
    m.placed = true;
}

void burst(const Ctx &ctx, AssetId<EmitterConfig> Fx::*which, Vec2 at, int count) {
    if (const Fx *fx = ctx.try_res<Fx>())
        ctx.emit(BurstAt{.config = fx->*which, .position = at, .count = static_cast<std::uint16_t>(count)});
}

void end_game(const Ctx &ctx, Mine &m, bool won) {
    m.won = won;
    ++m.games;
    if (won) {
        ++m.wins;
        const int par = m.mines * 12;                           // 120 s beginner, 480 s intermediate
        m.score += m.mines * 25 + std::max(0, par - seconds(m)) * (m.level + 1) * 2;
        for (int c = 0; c < m.w * m.h; ++c)                    // auto-flag the rest
            if (m.cells[static_cast<std::size_t>(c)] & kMine) m.cells[static_cast<std::size_t>(c)] |= kFlag;
        m.flags = m.mines;
        const Rect r = board_rect(m);
        for (int i = 0; i < 5; ++i)
            burst(ctx, &Fx::confetti, {r.x + r.w * (0.1f + 0.2f * static_cast<float>(i)), r.y + r.h * 0.4f}, 40);
        ctx.emit(ShakeAt{.trauma = 0.35f});
    } else {
        burst(ctx, &Fx::boom, cell_center(m, m.boom), 90);
        ctx.emit(ShakeAt{.trauma = 0.9f});
    }
    m.best = std::max(m.best, m.score);
    set_phase(m, kOver);
}

// Opens a cell and flood-fills zeros. Returns false on a mine.
bool reveal(const Ctx &ctx, Mine &m, int cell) {
    std::uint8_t &c0 = m.cells[static_cast<std::size_t>(cell)];
    if (c0 & (kOpen | kFlag)) return true;
    if (!m.placed) place_mines(m, ctx.rng("mines"_id), cell);
    if (c0 & kMine) {
        c0 |= kOpen;
        m.boom = cell;
        end_game(ctx, m, false);
        return false;
    }
    std::array<std::int16_t, kMaxCells> stack{};
    int top = 0, opened = 0;
    c0 |= kOpen;
    stack[static_cast<std::size_t>(top++)] = static_cast<std::int16_t>(cell);
    while (top > 0) {
        const int c = stack[static_cast<std::size_t>(--top)];
        ++opened;
        if ((m.cells[static_cast<std::size_t>(c)] & kCount) != 0) continue;
        each_neighbour(m, c, [&](int o) {
            std::uint8_t &oc = m.cells[static_cast<std::size_t>(o)];
            if (oc & (kOpen | kFlag | kMine)) return;
            oc |= kOpen;
            stack[static_cast<std::size_t>(top++)] = static_cast<std::int16_t>(o);
        });
    }
    m.opened += opened;
    m.score += opened * 10;
    burst(ctx, &Fx::spark, cell_center(m, cell), std::min(60, 6 + opened * 2));
    if (m.opened == m.w * m.h - m.mines) end_game(ctx, m, true);
    return true;
}

void toggle_flag(Mine &m, int cell) {
    std::uint8_t &c = m.cells[static_cast<std::size_t>(cell)];
    if (c & kOpen) return;
    c ^= kFlag;
    m.flags += (c & kFlag) ? 1 : -1;
}

// Chord: a number with all its flags placed opens its other neighbours.
void chord(const Ctx &ctx, Mine &m, int cell) {
    const std::uint8_t c = m.cells[static_cast<std::size_t>(cell)];
    if (!(c & kOpen) || (c & kCount) == 0) return;
    int flags = 0;
    each_neighbour(m, cell, [&](int o) { flags += (m.cells[static_cast<std::size_t>(o)] & kFlag) ? 1 : 0; });
    if (flags != (c & kCount)) return;
    each_neighbour(m, cell, [&](int o) {
        if (m.phase == kPlay && !(m.cells[static_cast<std::size_t>(o)] & (kOpen | kFlag))) reveal(ctx, m, o);
    });
}

// All input, for every phase.
void play(Ctx &ctx) {
    Mine &m = ctx.local<Mine>();
    const Controls &k = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    ++m.phase_ticks;
    const bool click = in.pointer_valid && in.pointer_pressed != 0;
    if (m.phase == kTitle) {
        if (in.is_pressed(k.one) || in.is_pressed(k.left)) m.level = 0;
        if (in.is_pressed(k.two) || in.is_pressed(k.right)) m.level = 1;
        if (in.is_pressed(k.up) || in.is_pressed(k.down)) m.level = 1 - m.level;
        bool start = in.is_pressed(k.confirm) || (in.is_pressed(k.reveal) && !click);
        if (click)
            for (int i = 0; i < 2; ++i)
                if (title_button(i).contains(in.pointer_virtual)) { m.level = i; start = true; }
        if (start) new_board(m);
        return;
    }
    if (m.phase == kOver) {
        if (in.is_pressed(k.restart)) new_board(m);
        else if (m.phase_ticks > 20 && (in.is_pressed(k.confirm) || in.is_pressed(k.reveal) || in.is_pressed(k.back)))
            set_phase(m, kTitle);
        return;
    }
    // kPlay
    if (in.is_pressed(k.back)) { set_phase(m, kTitle); return; }
    if (in.is_pressed(k.restart)) { new_board(m); return; }
    if (m.placed) ++m.ticks;
    if (in.is_repeated(k.left)) m.cx = std::max(0, m.cx - 1);
    if (in.is_repeated(k.right)) m.cx = std::min(m.w - 1, m.cx + 1);
    if (in.is_repeated(k.up)) m.cy = std::max(0, m.cy - 1);
    if (in.is_repeated(k.down)) m.cy = std::min(m.h - 1, m.cy + 1);
    if (click) {                                                // act on the cell under the pointer
        const int at = cell_at(m, in.pointer_virtual);
        if (at < 0) return;
        m.cx = at % m.w;
        m.cy = at / m.w;
    }
    const int cell = m.cy * m.w + m.cx;
    const bool open = m.cells[static_cast<std::size_t>(cell)] & kOpen;
    if (in.is_pressed(k.reveal)) open ? chord(ctx, m, cell) : (void)reveal(ctx, m, cell);
    else if (in.is_pressed(k.flag)) toggle_flag(m, cell);
    else if (in.is_pressed(k.chord)) chord(ctx, m, cell);
}

} // namespace

void start_world(World &w) {
    w.resources().insert<Mine>();
    w.snapshot_resource<Mine>();
}

void RulesPlugin::build(App &app) {
    app.add_event<BurstAt>();
    app.add_event<ShakeAt>();
    app.add_system({.stage = Stage::Input, .fn = play, .name = "minesweeper.play", .run_if = &has_local<Mine>});
}

} // namespace ms
