// --bot: dodge bombs, get under the lowest column or the UFO, fire.
#include <algorithm>
#include <cstdlib>
#include "invaders/rules.h"

namespace inv {
namespace {

// Danger at x: bombs above the cannon, the lower the worse.
int danger_at(const Run &r, int x) {
    int d = 0;
    for (const Shot &b : r.bombs)
        if (b.active && std::abs(b.x - x) <= kPlayerW / 2 + 4 && b.y > kPlayerY - 70) d += b.y;
    return d;
}

} // namespace

BotChoice bot_decide(const Run &r) {
    BotChoice c;
    if (r.dead_ticks > 0) return c;
    int target = r.player_x;
    int best_bottom = -1;
    for (int col = 0; col < kCols; ++col)
        for (int row = kRows - 1; row >= 0; --row)
            if (r.alive[static_cast<std::size_t>(row * kCols + col)] != 0) {
                const int bottom = inv_y(r.fy, row) * 4 - std::abs(inv_x(r.fx, col) + kInvW / 2 - r.player_x) / 8;
                if (bottom > best_bottom) {
                    best_bottom = bottom;
                    target = inv_x(r.fx, col) + kInvW / 2 + r.dir * 4;   // lead the march a little
                }
                break;
            }
    if (r.ufo_active && std::abs(r.ufo_x - r.player_x) < 80) target = r.ufo_x + kUfoW / 2 + r.ufo_dir * 24;
    target = std::clamp(target, 12, kW - 12);

    // Least danger first, then closest to the target.
    int best = 0, best_score = 1 << 30;
    for (int m = -1; m <= 1; ++m) {
        const int x = std::clamp(r.player_x + m * 6, 12, kW - 12);
        const int score = danger_at(r, x) * 100 + std::abs(target - x);
        if (score < best_score) {
            best_score = score;
            best = m;
        }
    }
    if (best == 0 && danger_at(r, r.player_x) == 0 && std::abs(target - r.player_x) > 1)
        best = target > r.player_x ? 1 : -1;
    c.move = best;
    c.fire = !r.shot.active && std::abs(target - r.player_x) < 8;
    return c;
}

void Bot::fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) {
    lam::InputFrame &f = frames[0];
    f.clear_devices();
    ++ticks_;
    const Run *r = ctx.try_local<Run>();
    if (r == nullptr) {                                         // title or game over
        if (ctx.focused() && ticks_ % 20 == 0) f.press(controls.confirm);
        return;
    }
    const BotChoice c = bot_decide(*r);
    if (c.move < 0) f.hold(controls.left);
    if (c.move > 0) f.hold(controls.right);
    if (c.fire) f.press(controls.fire);
}

} // namespace inv
