#include "invaders/rules.h"

#include <algorithm>
#include <cstdlib>
#include <lam/core/id.h>
#include <lam/math/rng.h>
#include <lam/render/camera_math.h>

namespace inv {
namespace {

using namespace lam::literals;

// 11 x 8 cells, 2 px each: an arch with a notch underneath.
constexpr const char *kShieldShape[kShieldCH] = {"..XXXXXXX..", ".XXXXXXXXX.", "XXXXXXXXXXX", "XXXXXXXXXXX",
                                                 "XXXXXXXXXXX", "XXXXXXXXXXX", "XXX.....XXX", "XX.......XX"};

struct Box {
    int x, y, w, h;
};
bool overlap(Box a, Box b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }

void burst(const lam::Ctx &ctx, Fx fx, int x, int y, int count) {
    if (const FxIds *ids = ctx.try_res<FxIds>())
        ctx.emit(lam::BurstAt{.config = ids->ids[fx], .position = {static_cast<float>(x), static_cast<float>(y)},
                              .count = static_cast<std::uint16_t>(count)});
}

Fx row_fx(int row) { return row == 0 ? kFxPink : row < 3 ? kFxCyan : kFxGreen; }

void reset_shields(Run &r) {
    for (int s = 0; s < kShields; ++s)
        for (int cy = 0; cy < kShieldCH; ++cy)
            for (int cx = 0; cx < kShieldCW; ++cx)
                r.shield[static_cast<std::size_t>(s * kShieldCells + cy * kShieldCW + cx)] = shield_solid(s, cx, cy) ? 1 : 0;
}

void start_wave(Run &r) {
    r.alive.fill(1);
    r.alive_count = kCount;
    r.fx = (kW - ((kCols - 1) * kCellW + kInvW)) / 2;
    r.fy = 40 + std::min(r.wave - 1, 5) * 8;
    r.dir = 1;
    r.march_timer = 30;
    r.drop_next = false;
    r.shot.active = false;
    for (Shot &b : r.bombs) b.active = false;
    r.bomb_timer = 90;
    reset_shields(r);
}

int march_interval(const Run &r) { return std::max(1, 1 + r.alive_count * 19 / kCount - (r.wave - 1)); }

// Clears the hit cell and a small crater around it (some corners at random).
void damage_shield(const lam::Ctx &ctx, Run &r, int s, int cx, int cy) {
    lam::Rng &rng = ctx.rng("inv.shield"_id);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= kShieldCW || y >= kShieldCH) continue;
            if (dx != 0 && dy != 0 && rng.below(2) == 0) continue;
            r.shield[static_cast<std::size_t>(s * kShieldCells + y * kShieldCW + x)] = 0;
        }
    burst(ctx, kFxChip, shield_x(s) + cx * kShieldCell + 1, kShieldY + cy * kShieldCell + 1, 5);
}

// The first solid shield cell under `b`, damaged; true if one was hit.
bool hit_shield(const lam::Ctx &ctx, Run &r, Box b) {
    for (int s = 0; s < kShields; ++s) {
        const int sx = shield_x(s);
        if (!overlap(b, {sx, kShieldY, kShieldCW * kShieldCell, kShieldCH * kShieldCell})) continue;
        for (int cy = 0; cy < kShieldCH; ++cy)
            for (int cx = 0; cx < kShieldCW; ++cx) {
                if (r.shield[static_cast<std::size_t>(s * kShieldCells + cy * kShieldCW + cx)] == 0) continue;
                if (!overlap(b, {sx + cx * kShieldCell, kShieldY + cy * kShieldCell, kShieldCell, kShieldCell})) continue;
                damage_shield(ctx, r, s, cx, cy);
                return true;
            }
    }
    return false;
}

void kill_player(const lam::Ctx &ctx, Run &r) {
    r.dead_ticks = 100;
    r.shot.active = false;
    for (Shot &b : r.bombs) b.active = false;
    burst(ctx, kFxPlayer, r.player_x, kPlayerY + 4, 60);
    ctx.emit(lam::ShakeAt{.trauma = 0.8f});
}

void move_player_shot(const lam::Ctx &ctx, Run &r) {
    Shot &s = r.shot;
    for (int i = 0; i < 5 && s.active; ++i) {
        --s.y;
        const Box b{s.x, s.y, 1, 4};
        if (s.y < 12) { s.active = false; break; }
        for (Shot &bomb : r.bombs)                              // shots and bombs cancel
            if (bomb.active && overlap(b, {bomb.x - 1, bomb.y, 3, 6})) {
                bomb.active = false;
                s.active = false;
                burst(ctx, kFxChip, s.x, s.y, 6);
            }
        if (!s.active) break;
        if (r.ufo_active && overlap(b, {r.ufo_x, kUfoY, kUfoW, kUfoH})) {
            static constexpr int kUfoPoints[4] = {50, 100, 150, 300};
            r.popup_value = kUfoPoints[ctx.rng("inv.ufo"_id).below(4)];
            r.popup_x = r.ufo_x + kUfoW / 2;
            r.popup_ticks = 90;
            r.score += r.popup_value;
            r.ufo_active = false;
            s.active = false;
            burst(ctx, kFxUfo, r.popup_x, kUfoY + 3, 40);
            ctx.emit(lam::ShakeAt{.trauma = 0.4f});
            break;
        }
        for (int i2 = 0; i2 < kCount && s.active; ++i2) {
            if (r.alive[static_cast<std::size_t>(i2)] == 0) continue;
            const int row = i2 / kCols, col = i2 % kCols;
            const Box ib{inv_x(r.fx, col), inv_y(r.fy, row), kInvW, kInvH};
            if (!overlap(b, ib)) continue;
            r.alive[static_cast<std::size_t>(i2)] = 0;
            --r.alive_count;
            ++r.kills;
            r.score += row_points(row);
            s.active = false;
            burst(ctx, row_fx(row), ib.x + kInvW / 2, ib.y + kInvH / 2, 18);
        }
        if (s.active && hit_shield(ctx, r, b)) s.active = false;
    }
}

void move_bombs(const lam::Ctx &ctx, Run &r) {
    for (Shot &bomb : r.bombs) {
        const int speed = bomb.kind == 0 ? 2 : 3;
        for (int i = 0; i < speed && bomb.active; ++i) {
            ++bomb.y;
            const Box b{bomb.x - 1, bomb.y, 3, 6};
            if (bomb.y + 6 >= kGroundY) {
                bomb.active = false;
                burst(ctx, kFxChip, bomb.x, kGroundY, 4);
            } else if (hit_shield(ctx, r, b)) {
                bomb.active = false;
            } else if (r.dead_ticks == 0 &&
                       overlap(b, {r.player_x - kPlayerW / 2, kPlayerY + 2, kPlayerW, kPlayerH - 2})) {
                bomb.active = false;
                --r.lives;
                kill_player(ctx, r);
                return;
            }
        }
    }
}

void march(const lam::Ctx &ctx, Run &r) {
    if (--r.march_timer > 0) return;
    r.march_timer = march_interval(r);
    r.anim ^= 1;
    if (r.drop_next) {
        r.fy += 8;
        r.dir = -r.dir;
        r.drop_next = false;
    } else {
        r.fx += 3 * r.dir;
    }
    int min_c = kCols, max_c = -1, max_r = -1;
    for (int i = 0; i < kCount; ++i)
        if (r.alive[static_cast<std::size_t>(i)] != 0) {
            min_c = std::min(min_c, i % kCols);
            max_c = std::max(max_c, i % kCols);
            max_r = std::max(max_r, i / kCols);
        }
    if (max_c < 0) return;
    const int left = inv_x(r.fx, min_c), right = inv_x(r.fx, max_c) + kInvW;
    if ((r.dir > 0 && right + 3 > kW - 6) || (r.dir < 0 && left - 3 < 6)) r.drop_next = true;
    // Invaders eat through the shields they overlap.
    for (int i = 0; i < kCount; ++i) {
        if (r.alive[static_cast<std::size_t>(i)] == 0) continue;
        const Box ib{inv_x(r.fx, i % kCols), inv_y(r.fy, i / kCols), kInvW, kInvH};
        for (int s = 0; s < kShields; ++s)
            for (int c = 0; c < kShieldCells; ++c) {
                const Box cb{shield_x(s) + (c % kShieldCW) * kShieldCell, kShieldY + (c / kShieldCW) * kShieldCell,
                             kShieldCell, kShieldCell};
                if (overlap(ib, cb)) r.shield[static_cast<std::size_t>(s * kShieldCells + c)] = 0;
            }
    }
    if (inv_y(r.fy, max_r) + kInvH >= kPlayerY) {               // landed
        r.lives = 0;
        kill_player(ctx, r);
    }
}

void drop_bombs(const lam::Ctx &ctx, Run &r) {
    if (--r.bomb_timer > 0) return;
    lam::Rng &rng = ctx.rng("inv.bombs"_id);
    r.bomb_timer = std::max(10, 48 - r.wave * 4 - (kCount - r.alive_count) / 2) + rng.between(0, 30);
    Shot *slot = nullptr;
    for (Shot &b : r.bombs)
        if (!b.active) slot = &b;
    if (slot == nullptr) return;
    int col = -1;
    if (rng.below(3) == 0) col = std::clamp((r.player_x - r.fx) / kCellW, 0, kCols - 1);   // aimed
    else col = rng.between(0, kCols - 1);
    for (int tries = 0; tries < kCols; ++tries, col = (col + 1) % kCols)
        for (int row = kRows - 1; row >= 0; --row)
            if (r.alive[static_cast<std::size_t>(row * kCols + col)] != 0) {
                *slot = Shot{.x = inv_x(r.fx, col) + kInvW / 2, .y = inv_y(r.fy, row) + kInvH,
                             .kind = static_cast<std::int32_t>(rng.below(2)), .active = true};
                return;
            }
}

void ufo(const lam::Ctx &ctx, Run &r) {
    if (r.popup_ticks > 0) --r.popup_ticks;
    if (!r.ufo_active) {
        if (--r.ufo_timer > 0 || r.alive_count < 8) return;
        lam::Rng &rng = ctx.rng("inv.ufo"_id);
        r.ufo_timer = 900 + rng.between(0, 600);
        r.ufo_dir = rng.below(2) == 0 ? 1 : -1;
        r.ufo_x = r.ufo_dir > 0 ? -kUfoW : kW;
        r.ufo_active = true;
        return;
    }
    r.ufo_x += r.ufo_dir;
    if (r.ufo_x < -kUfoW - 1 || r.ufo_x > kW + 1) r.ufo_active = false;
}

} // namespace

bool shield_solid(int /*s*/, int cx, int cy) { return kShieldShape[cy][cx] == 'X'; }

void start_run(lam::World &w) {
    Run &r = w.resources().insert<Run>();
    w.snapshot_resource<Run>();
    start_wave(r);
}

void step(lam::Ctx &ctx) {
    Run &r = ctx.local<Run>();
    if (r.over) return;
    const Controls &c = ctx.res<Controls>();
    const lam::InputFrame &in = lam::input_frame(ctx);

    if (r.dead_ticks > 0) {                                     // freeze while the cannon explodes
        if (--r.dead_ticks == 0) {
            if (r.lives <= 0) r.over = true;
            else r.player_x = kW / 2;
        }
        return;
    }
    const int move = (in.is_held(c.right) ? 1 : 0) - (in.is_held(c.left) ? 1 : 0);
    r.player_x = std::clamp(r.player_x + move * 2, 12, kW - 12);
    if (in.is_pressed(c.fire) && !r.shot.active && r.wave_pause == 0)
        r.shot = Shot{.x = r.player_x, .y = kPlayerY - 4, .kind = 0, .active = true};

    if (r.wave_pause > 0) {
        if (--r.wave_pause == 0) start_wave(r);
        return;
    }
    move_player_shot(ctx, r);
    move_bombs(ctx, r);
    if (r.dead_ticks > 0) return;
    march(ctx, r);
    if (r.dead_ticks > 0) return;
    drop_bombs(ctx, r);
    ufo(ctx, r);
    if (r.alive_count == 0) {
        ++r.wave;
        r.wave_pause = 120;
        r.shot.active = false;
        r.ufo_active = false;
        for (Shot &b : r.bombs) b.active = false;
    }
}

void RulesPlugin::build(lam::App &app) {
    app.add_event<lam::BurstAt>();
    app.add_event<lam::ShakeAt>();
    app.add_system({.stage = lam::Stage::Simulate, .fn = step, .name = "invaders.step", .run_if = &lam::has_local<Run>});
}

} // namespace inv
