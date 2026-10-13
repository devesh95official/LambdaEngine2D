// --bot: clicks cells like a mouse player, decided from Mine alone.
#include <algorithm>
#include <cstdint>
#include <lam/ecs/world.h>
#include <lam/input/keys.h>
#include "minesweeper/rules.h"

namespace ms {
using namespace lam;

namespace {
struct Around { int cells[8]; int n = 0; int need = 0; };       // hidden unflagged neighbours, mines left

Around around(const Mine &m, int cell) {
    Around a;
    const int x = cell % m.w, y = cell / m.w;
    int flags = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int nx = x + dx, ny = y + dy;
            if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= m.w || ny >= m.h) continue;
            const std::uint8_t c = m.cells[static_cast<std::size_t>(ny * m.w + nx)];
            if (c & kFlag) ++flags;
            else if (!(c & kOpen)) a.cells[a.n++] = ny * m.w + nx;
        }
    a.need = (m.cells[static_cast<std::size_t>(cell)] & kCount) - flags;
    return a;
}
bool has(const Around &a, int c) {
    for (int i = 0; i < a.n; ++i) if (a.cells[i] == c) return true;
    return false;
}
bool is_number(const Mine &m, int c) {
    const std::uint8_t v = m.cells[static_cast<std::size_t>(c)];
    return (v & kOpen) && (v & kCount) != 0;
}
} // namespace

Move bot_decide(const Mine &m) {
    const int n = m.w * m.h;
    if (!m.placed) return {1, (m.h / 2) * m.w + m.w / 2};
    for (int c = 0; c < n; ++c) {                               // single-cell rules
        if (!is_number(m, c)) continue;
        const Around a = around(m, c);
        if (a.n == 0) continue;
        if (a.need == 0) return {3, c};
        if (a.need == a.n) return {2, a.cells[0]};
    }
    for (int c = 0; c < n; ++c) {                               // subset rule: A's unknowns are a subset of B's
        if (!is_number(m, c)) continue;
        const Around a = around(m, c);
        if (a.n == 0) continue;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const int bx = c % m.w + dx, by = c / m.w + dy;
                if ((dx == 0 && dy == 0) || bx < 0 || by < 0 || bx >= m.w || by >= m.h) continue;
                const int b = by * m.w + bx;
                if (!is_number(m, b)) continue;
                const Around bb = around(m, b);
                bool subset = bb.n > a.n;
                for (int i = 0; subset && i < a.n; ++i) subset = has(bb, a.cells[i]);
                if (!subset) continue;
                const int rest = bb.need - a.need, extra = bb.n - a.n;
                for (int i = 0; i < bb.n; ++i) {
                    if (has(a, bb.cells[i])) continue;
                    if (rest == 0) return {1, bb.cells[i]};
                    if (rest == extra) return {2, bb.cells[i]};
                }
            }
    }
    // Guess the hidden cell with the lowest local mine estimate. Ties go to a fixed scramble.
    int hidden = 0;
    for (int c = 0; c < n; ++c) hidden += (m.cells[static_cast<std::size_t>(c)] & (kOpen | kFlag)) ? 0 : 1;
    const float density = hidden > 0 ? static_cast<float>(m.mines - m.flags) / static_cast<float>(hidden) : 1.0f;
    Move best;
    float best_p = 2;
    std::uint32_t best_key = 0;
    for (int c = 0; c < n; ++c) {
        if (m.cells[static_cast<std::size_t>(c)] & (kOpen | kFlag)) continue;
        float p = -1;
        const int x = c % m.w, y = c / m.w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= m.w || ny >= m.h || !is_number(m, ny * m.w + nx)) continue;
                const Around a = around(m, ny * m.w + nx);
                if (a.n > 0) p = std::max(p, static_cast<float>(a.need) / static_cast<float>(a.n));
            }
        if (p < 0) p = density;
        const std::uint32_t key = (static_cast<std::uint32_t>(c) * 2654435761u) ^ static_cast<std::uint32_t>(m.games * 7919 + m.opened);
        if (p < best_p - 1e-4f || (p < best_p + 1e-4f && key < best_key)) { best_p = p; best = {1, c}; best_key = key; }
    }
    return best;
}

void Bot::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    InputFrame &f = frames[0];
    f.clear_devices();
    if (!ctx.focused()) return;
    const Mine *m = ctx.world().resources().try_get<Mine>();
    if (m == nullptr) return;
    const auto click = [&](Vec2 at, MouseButton b, ButtonAction a) {
        f.pointer_valid = true;
        f.pointer_virtual = f.pointer_world = at;
        f.pointer_pressed = f.pointer_down = static_cast<std::uint8_t>(1u << static_cast<unsigned>(b));
        f.press(a);
    };
    if (m->phase == kTitle) {
        if (m->phase_ticks == 40) {                             // every third game is intermediate
            const Rect r = title_button(m->games % 3 == 2 ? 1 : 0);
            click({r.x + r.w / 2, r.y + r.h / 2}, MouseButton::Left, c.reveal);
        }
    } else if (m->phase == kOver) {
        if (m->phase_ticks == 100) f.press(m->games % 2 == 0 ? c.confirm : c.restart);
    } else if (m->phase_ticks % 5 == 4) {
        const Move mv = bot_decide(*m);
        if (mv.kind == 0) return;
        const Vec2 at = cell_center(*m, mv.cell);
        if (mv.kind == 1) click(at, MouseButton::Left, c.reveal);
        else if (mv.kind == 2) click(at, MouseButton::Right, c.flag);
        else click(at, MouseButton::Middle, c.chord);
    }
}

} // namespace ms
