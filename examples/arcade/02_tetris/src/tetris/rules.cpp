#include "tetris/rules.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include <lam/core/id.h>
#include <lam/math/rng.h>
#include <lam/render/camera_math.h>

namespace tetris {
namespace {

using lam::IVec2;
constexpr lam::StrId kBagStream = lam::str_id("tetris.bag");

// Spawn-rotation cells (y down) and box size per kind. Other rotations turn inside the box.
struct Shape { std::array<IVec2, 4> c; int n; };
constexpr std::array<Shape, 8> kShapes{{
    {{{{0, 0}, {0, 0}, {0, 0}, {0, 0}}}, 0},
    {{{{0, 1}, {1, 1}, {2, 1}, {3, 1}}}, 4},                    // I
    {{{{0, 0}, {1, 0}, {0, 1}, {1, 1}}}, 2},                    // O
    {{{{1, 0}, {0, 1}, {1, 1}, {2, 1}}}, 3},                    // T
    {{{{1, 0}, {2, 0}, {0, 1}, {1, 1}}}, 3},                    // S
    {{{{0, 0}, {1, 0}, {1, 1}, {2, 1}}}, 3},                    // Z
    {{{{0, 0}, {0, 1}, {1, 1}, {2, 1}}}, 3},                    // J
    {{{{2, 0}, {0, 1}, {1, 1}, {2, 1}}}, 3},                    // L
}};
// SRS clockwise kicks from rotation r, y up as in the guideline tables. Counter-clockwise from r
// is the negated clockwise kick from r-1.
constexpr IVec2 kKickJLSTZ[4][5] = {{{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}},
                                    {{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}},
                                    {{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}},
                                    {{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}};
constexpr IVec2 kKickI[4][5] = {{{0, 0}, {-2, 0}, {1, 0}, {-2, -1}, {1, 2}},
                                {{0, 0}, {-1, 0}, {2, 0}, {-1, 2}, {2, -1}},
                                {{0, 0}, {2, 0}, {-1, 0}, {2, 1}, {-1, -2}},
                                {{0, 0}, {1, 0}, {-2, 0}, {1, -2}, {-2, 1}}};
constexpr int kLinePoints[5] = {0, 100, 300, 500, 800};

std::uint8_t draw_kind(Board &b, lam::Rng &rng) {
    if (b.bag_left == 0) {                                      // refill and shuffle the 7-bag
        for (int i = 0; i < 7; ++i) b.bag[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i + 1);
        for (int i = 6; i > 0; --i) std::swap(b.bag[static_cast<std::size_t>(i)], b.bag[rng.below(static_cast<std::uint32_t>(i + 1))]);
        b.bag_left = 7;
    }
    return b.bag[static_cast<std::size_t>(7 - b.bag_left--)];
}

std::uint8_t pull_next(Board &b, lam::Rng &rng) {
    const std::uint8_t k = b.next[0];
    for (std::size_t i = 0; i + 1 < b.next.size(); ++i) b.next[i] = b.next[i + 1];
    b.next.back() = draw_kind(b, rng);
    return k;
}

void spawn(Board &b, std::uint8_t kind) {
    b.kind = kind;
    b.rot = 0;
    b.x = kind == kO ? 4 : 3;
    b.y = kind == kI ? -1 : 0;
    b.fall_ticks = b.lock_ticks = b.lock_resets = 0;
    ++b.pieces;
    if (!fits(b, b.kind, b.rot, b.x, b.y)) b.over = true;       // block out
}

lam::Vec2 cell_centre(float x, float y) { return {kOrigin.x + (x + 0.5f) * kCell, kOrigin.y + (y + 0.5f) * kCell}; }

void burst(const lam::Ctx &ctx, bool spark, lam::Vec2 at, std::uint16_t count) {
    if (const Fx *fx = ctx.try_res<Fx>()) ctx.emit(lam::BurstAt{.config = spark ? fx->spark : fx->dust, .position = at, .count = count});
}

bool grounded(const Board &b) { return !fits(b, b.kind, b.rot, b.x, b.y + 1); }

// Moving or turning on the ground resets the lock delay, up to a limit.
void moved(Board &b) {
    if (grounded(b) && b.lock_resets < kMaxLockResets) {
        b.lock_ticks = 0;
        ++b.lock_resets;
    }
}

bool try_move(Board &b, int dx) {
    if (!fits(b, b.kind, b.rot, b.x + dx, b.y)) return false;
    b.x += dx;
    moved(b);
    return true;
}

bool try_rotate(Board &b, int dir) {
    if (b.kind == kO) return true;
    const int to = (b.rot + (dir > 0 ? 1 : 3)) % 4;
    const auto &table = b.kind == kI ? kKickI : kKickJLSTZ;
    for (int i = 0; i < 5; ++i) {
        const IVec2 k = dir > 0 ? table[b.rot][i] : IVec2{-table[to][i].x, -table[to][i].y};
        if (fits(b, b.kind, to, b.x + k.x, b.y - k.y)) {
            b.rot = static_cast<std::uint8_t>(to);
            b.x += k.x;
            b.y -= k.y;
            moved(b);
            return true;
        }
    }
    return false;
}

void game_over(const lam::Ctx &ctx, Board &b) {
    b.over = true;
    b.kind = kNone;
    ctx.emit(lam::ShakeAt{.trauma = 0.7f});
    for (int i = 0; i < 4; ++i) burst(ctx, false, cell_centre(1.5f + 2.0f * static_cast<float>(i), 1), 16);
}

void lock(const lam::Ctx &ctx, Board &b) {
    bool above = false;
    for (const IVec2 c : cells_of(b.kind, b.rot)) {
        const int cx = b.x + c.x, cy = b.y + c.y;
        if (cy < 0) { above = true; continue; }
        b.cells[static_cast<std::size_t>(cy * kW + cx)] = b.kind;
    }
    b.kind = kNone;
    b.can_hold = true;
    if (above) { game_over(ctx, b); return; }                   // lock out
    int n = 0;
    for (int y = 0; y < kH; ++y) {
        bool full = true;
        for (int x = 0; x < kW && full; ++x) full = b.cells[static_cast<std::size_t>(y * kW + x)] != 0;
        if (!full) continue;
        b.clear_mask |= 1u << y;
        ++n;
        for (int i = 0; i < 5; ++i) burst(ctx, true, cell_centre(1.0f + 2.0f * static_cast<float>(i), static_cast<float>(y)), 7);
    }
    if (n == 0) {
        spawn(b, pull_next(b, ctx.rng(kBagStream)));
        if (b.over) game_over(ctx, b);
        return;
    }
    b.flash = kFlashTicks;
    b.score += kLinePoints[n] * b.level;
    b.lines += n;
    b.level = 1 + b.lines / 10;
    if (n == 4) ++b.tetrises;
    ctx.emit(lam::ShakeAt{.trauma = n == 4 ? 0.8f : 0.25f + 0.08f * static_cast<float>(n)});
}

void collapse(Board &b) {
    int dst = kH - 1;
    for (int y = kH - 1; y >= 0; --y) {
        if (b.clear_mask & (1u << y)) continue;
        for (int x = 0; x < kW; ++x) b.cells[static_cast<std::size_t>(dst * kW + x)] = b.cells[static_cast<std::size_t>(y * kW + x)];
        --dst;
    }
    for (; dst >= 0; --dst)
        for (int x = 0; x < kW; ++x) b.cells[static_cast<std::size_t>(dst * kW + x)] = 0;
    b.clear_mask = 0;
}

// Input, gravity, lock delay and line clears, in that order.
void step(lam::Ctx &ctx) {
    Board &b = ctx.local<Board>();
    if (b.over) return;
    if (b.flash > 0) {
        if (--b.flash == 0) {
            collapse(b);
            spawn(b, pull_next(b, ctx.rng(kBagStream)));
            if (b.over) game_over(ctx, b);
        }
        return;
    }
    if (b.kind == kNone) return;
    const Controls *c = ctx.try_res<Controls>();
    const lam::InputFrame &in = lam::input_frame(ctx);
    bool soft = false;
    if (c != nullptr) {
        if (in.is_pressed(c->hold) && b.can_hold) {
            const std::uint8_t cur = b.kind;
            spawn(b, b.hold != kNone ? b.hold : pull_next(b, ctx.rng(kBagStream)));
            b.hold = cur;
            b.can_hold = false;
            if (b.over) { game_over(ctx, b); return; }
        }
        if (in.is_pressed(c->rot_cw)) try_rotate(b, 1);
        if (in.is_pressed(c->rot_ccw)) try_rotate(b, -1);
        if (in.is_repeated(c->left)) try_move(b, -1);
        if (in.is_repeated(c->right)) try_move(b, 1);
        if (in.is_pressed(c->drop)) {
            const int d = drop_distance(b);
            b.y += d;
            b.score += 2 * d;
            ctx.emit(lam::ShakeAt{.trauma = 0.3f});
            const auto cs = cells_of(b.kind, b.rot);
            for (const IVec2 cc : cs) burst(ctx, false, cell_centre(static_cast<float>(b.x + cc.x), static_cast<float>(b.y + cc.y) + 0.5f), 3);
            lock(ctx, b);
            return;
        }
        soft = in.is_held(c->soft);
    }
    const int interval = soft ? std::min(gravity_ticks(b.level), 2) : gravity_ticks(b.level);
    if (++b.fall_ticks >= interval) {
        b.fall_ticks = 0;
        if (!grounded(b)) {
            ++b.y;
            if (soft) b.score += 1;
        }
    }
    if (grounded(b)) {
        if (++b.lock_ticks >= kLockTicks) lock(ctx, b);
    } else {
        b.lock_ticks = 0;
    }
}

} // namespace

std::array<IVec2, 4> cells_of(int kind, int rot) {
    const Shape &s = kShapes[static_cast<std::size_t>(kind)];
    std::array<IVec2, 4> out = s.c;
    if (s.n < 3) return out;
    for (int r = 0; r < (rot & 3); ++r)
        for (IVec2 &p : out) p = IVec2{s.n - 1 - p.y, p.x};    // clockwise inside the n x n box
    return out;
}

bool fits(const Board &b, int kind, int rot, int x, int y) {
    for (const IVec2 c : cells_of(kind, rot)) {
        const int cx = x + c.x, cy = y + c.y;
        if (cx < 0 || cx >= kW || cy >= kH) return false;
        if (cy >= 0 && b.cells[static_cast<std::size_t>(cy * kW + cx)] != 0) return false;
    }
    return true;
}

int drop_distance(const Board &b) {
    int d = 0;
    while (fits(b, b.kind, b.rot, b.x, b.y + d + 1)) ++d;
    return d;
}

int gravity_ticks(int level) {
    constexpr int kTable[] = {60, 48, 37, 28, 21, 16, 11, 8, 6, 4, 3, 2, 2, 1};
    const int i = level - 1;
    return i < 0 ? kTable[0] : i >= static_cast<int>(std::size(kTable)) ? 1 : kTable[i];
}

void start_run(lam::World &w) {
    w.resources().insert<Board>();
    w.snapshot_resource<Board>();
    Board &b = w.resources().get<Board>();
    lam::Rng &rng = w.rng(kBagStream);
    for (auto &k : b.next) k = draw_kind(b, rng);
    spawn(b, pull_next(b, rng));
}

void RulesPlugin::build(lam::App &app) {
    app.add_event<lam::BurstAt>();
    app.add_event<lam::ShakeAt>();
    app.add_system({.stage = lam::Stage::Simulate, .fn = step, .name = "tetris.step", .run_if = lam::has_local<Board>});
}

} // namespace tetris
