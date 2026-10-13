#include "tetris/bot.h"

#include <algorithm>
#include <cstdlib>

namespace tetris {
namespace {

double evaluate(std::array<std::uint8_t, kW * kH> cells) {
    int lines = 0;
    for (int y = kH - 1; y >= 0; --y) {
        bool full = true;
        for (int x = 0; x < kW && full; ++x) full = cells[static_cast<std::size_t>(y * kW + x)] != 0;
        if (!full) continue;
        ++lines;
        for (int yy = y; yy > 0; --yy)
            for (int x = 0; x < kW; ++x) cells[static_cast<std::size_t>(yy * kW + x)] = cells[static_cast<std::size_t>((yy - 1) * kW + x)];
        for (int x = 0; x < kW; ++x) cells[static_cast<std::size_t>(x)] = 0;
        ++y;                                                    // re-check the row that fell in
    }
    int agg = 0, holes = 0, bump = 0, prev = -1;
    for (int x = 0; x < kW; ++x) {
        int h = 0;
        for (int y = 0; y < kH; ++y) {
            if (cells[static_cast<std::size_t>(y * kW + x)] != 0) { if (h == 0) h = kH - y; }
            else if (h > 0) ++holes;
        }
        agg += h;
        if (prev >= 0) bump += std::abs(h - prev);
        prev = h;
    }
    return -0.510066 * agg + 0.760666 * lines - 0.35663 * holes - 0.184483 * bump;
}

} // namespace

Plan best_placement(const Board &b, int kind) {
    Plan best;
    const int y0 = kind == kI ? -1 : 0;
    for (int rot = 0; rot < (kind == kO ? 1 : 4); ++rot) {
        for (int x = -2; x < kW; ++x) {
            if (!fits(b, kind, rot, x, y0)) continue;
            int y = y0;
            while (fits(b, kind, rot, x, y + 1)) ++y;
            std::array<std::uint8_t, kW * kH> cells = b.cells;
            bool above = false;
            for (const lam::IVec2 c : cells_of(kind, rot)) {
                if (y + c.y < 0) { above = true; break; }
                cells[static_cast<std::size_t>((y + c.y) * kW + x + c.x)] = static_cast<std::uint8_t>(kind);
            }
            const double s = above ? -1e9 : evaluate(cells);
            if (s > best.score) best = Plan{rot, x, s};
        }
    }
    return best;
}

void Bot::fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) {
    lam::InputFrame &f = frames[0];
    f.clear_devices();
    lam::World &w = ctx.world();
    const Board *b = w.resources().try_get<Board>();
    if (b == nullptr) {                                         // title or game over
        if (w.tick() >= 60) f.press(c_.confirm);
        return;
    }
    if (b->kind == kNone || b->over || w.tick() % 2 != 0) return;   // one action every other tick
    if (reckless_) { f.press(c_.drop); return; }
    if (b->pieces != key_) {
        key_ = b->pieces;
        actions_ = 0;
        plan_ = best_placement(*b, b->kind);
        use_hold_ = false;
        if (b->can_hold) {
            const int alt = b->hold != kNone ? b->hold : b->next[0];
            if (best_placement(*b, alt).score > plan_.score + 0.5) use_hold_ = true;
        }
    }
    if (use_hold_) { f.press(c_.hold); use_hold_ = false; return; }
    if (++actions_ > 14) { f.press(c_.drop); return; }        // stuck, just drop
    if (b->rot != plan_.rot) f.press((plan_.rot - b->rot + 4) % 4 == 3 ? c_.rot_ccw : c_.rot_cw);
    else if (b->x < plan_.x) f.press(c_.right);
    else if (b->x > plan_.x) f.press(c_.left);
    else f.press(c_.drop);
}

} // namespace tetris
