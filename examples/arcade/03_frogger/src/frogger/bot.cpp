// --bot: hop forward when the next cell stays safe a little longer than a hop takes.
// Otherwise wait, step sideways or back off.
#include <frogger/frogger.h>

#include <algorithm>
#include <cstdlib>
#include <lam/ecs/world.h>

namespace frogger {
using namespace lam;

void Bot::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    World &w = ctx.world();
    const Controls *c = ctx.try_res<Controls>();
    const Run *run = w.resources().try_get<Run>();
    if (c == nullptr || run == nullptr) return;
    InputFrame &f = frames[0];
    f.clear_devices();
    if (run->state == kTitle || run->state == kOver) {
        if (w.tick() % 30 == 0) f.press(c->start);
        return;
    }
    if (run->state != kPlay || run->hop > 0) return;
    constexpr int H = kHopTicks + 6;
    const auto side = [&](int dx) {
        return std::clamp(run->cx + dx * kCell * kMilli, (kCell / 2) * kMilli, (kW - kCell / 2) * kMilli);
    };
    const int row = run->row;
    const bool stay_ok = safe_for(w, *run, row, run->cx, H);
    if (row > 0 && safe_for(w, *run, row - 1, run->cx, H)) {
        f.press(c->up);
        return;
    }
    if (row == 1) {                                             // line up under a free home
        int best = -1;
        for (int k = 0; k < kHomes; ++k)
            if (!(run->homes & (1u << k)) &&
                (best < 0 || std::abs(slot_cx(k) * kMilli - run->cx) < std::abs(slot_cx(best) * kMilli - run->cx)))
                best = k;
        const std::int32_t d = best < 0 ? 0 : slot_cx(best) * kMilli - run->cx;
        if (std::abs(d) > 10 * kMilli) {
            const int dx = d > 0 ? 1 : -1;
            if (safe_for(w, *run, row, side(dx), H)) {
                f.press(dx > 0 ? c->right : c->left);
                return;
            }
        }
    }
    if (stay_ok) return;
    const int toward_mid = run->cx < (kW / 2) * kMilli ? 1 : -1;
    for (int dx : {toward_mid, -toward_mid})
        if (safe_for(w, *run, row, side(dx), H)) {
            f.press(dx > 0 ? c->right : c->left);
            return;
        }
    if (row < kStartRow && safe_for(w, *run, row + 1, run->cx, H)) f.press(c->down);
}
} // namespace frogger
