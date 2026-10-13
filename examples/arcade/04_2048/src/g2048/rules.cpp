#include "g2048/rules.h"

#include <algorithm>
#include <cmath>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/particles/components.h>
#include <lam/render/camera_math.h>

namespace g2048 {

using namespace lam::literals;

namespace {

// The cells of line `l` in `dir`, starting at the edge the tiles slide toward.
int line_cell(int dir, int l, int k) {
    switch (dir) {
    case kUp: return k * kN + l;
    case kDown: return (kN - 1 - k) * kN + l;
    case kLeft: return l * kN + k;
    default: return l * kN + (kN - 1 - k);
    }
}

int empties(const Cells &c) { return static_cast<int>(std::count(c.begin(), c.end(), 0)); }

}  // namespace

Slide slide(const Cells &cells, int dir) {
    Slide s;
    s.src_a.fill(-1);
    s.src_b.fill(-1);
    for (int l = 0; l < kN; ++l) {
        int out = -1;                                           // last filled slot of this line
        bool last_merged = false;
        for (int k = 0; k < kN; ++k) {
            const int from = line_cell(dir, l, k);
            const std::int32_t v = cells[from];
            if (v == 0) continue;
            if (out >= 0 && !last_merged && s.cells[line_cell(dir, l, out)] == v) {
                const int to = line_cell(dir, l, out);
                s.cells[to] = v * 2;
                s.src_b[to] = from;
                s.gained += v * 2;
                ++s.merges;
                last_merged = true;
                s.moved = true;
                continue;
            }
            const int to = line_cell(dir, l, ++out);
            s.cells[to] = v;
            s.src_a[to] = from;
            last_merged = false;
            if (to != from) s.moved = true;
        }
    }
    return s;
}

bool can_move(const Cells &c) {
    for (int i = 0; i < kCells; ++i) {
        if (c[i] == 0) return true;
        if (i % kN < kN - 1 && c[i] == c[i + 1]) return true;
        if (i / kN < kN - 1 && c[i] == c[i + kN]) return true;
    }
    return false;
}

int max_tile(const Cells &c) { return *std::max_element(c.begin(), c.end()); }

int best_dir(const Cells &cells) {
    // Points, free cells, and the biggest tile kept in the bottom-left corner.
    int best = -1;
    double best_score = -1e18;
    for (int d : {kDown, kLeft, kRight, kUp}) {
        const Slide s = slide(cells, d);
        if (!s.moved) continue;
        double score = s.gained + 12.0 * empties(s.cells);
        const int top = max_tile(s.cells);
        if (s.cells[(kN - 1) * kN] == top) score += 2.0 * top;
        for (int i = 0; i < kCells; ++i)                        // reward rows that grow to the left
            if (i % kN < kN - 1 && s.cells[i] >= s.cells[i + 1]) score += 1.0 + std::log2(1.0 + s.cells[i]);
        if (d == kUp) score -= 200;                             // last resort
        if (score > best_score) {
            best_score = score;
            best = d;
        }
    }
    return best;
}

namespace {

void spawn_tile(Game &g, lam::Rng &rng) {
    int free_cells[kCells];
    int n = 0;
    for (int i = 0; i < kCells; ++i)
        if (g.cells[i] == 0) free_cells[n++] = i;
    if (n == 0) {
        g.spawned = -1;
        return;
    }
    const int at = free_cells[rng.below(static_cast<std::uint32_t>(n))];
    g.cells[at] = rng.below(10) == 0 ? 4 : 2;
    g.spawned = at;
}

void new_game(Game &g, lam::Rng &rng) {
    const std::int32_t best = g.best, games = g.games;
    g = Game{};
    g.best = best;
    g.games = games + 1;
    g.phase = Phase::Play;
    g.src_a.fill(-1);
    g.src_b.fill(-1);
    spawn_tile(g, rng);
    const std::int32_t first = g.spawned;
    spawn_tile(g, rng);
    g.src_a[first] = -2;                                        // -2: pop in
    g.since_move = kSlideTicks;                                 // no slide, just the pops
}

void set_phase(Game &g, Phase p) {
    g.phase = p;
    g.phase_ticks = 0;
}

void do_move(lam::Ctx &ctx, Game &g, int dir) {
    const Slide s = slide(g.cells, dir);
    if (!s.moved) return;
    g.prev = g.cells;
    g.prev_score = g.score;
    g.can_undo = true;
    g.cells = s.cells;
    g.src_a = s.src_a;
    g.src_b = s.src_b;
    g.score += s.gained;
    g.best = std::max(g.best, g.score);
    ++g.moves;
    g.since_move = 0;
    spawn_tile(g, ctx.rng("spawn"_id));
}

void undo(Game &g) {
    if (!g.can_undo) return;
    g.cells = g.prev;
    g.score = g.prev_score;
    g.can_undo = false;
    g.src_a.fill(-1);
    g.src_b.fill(-1);
    g.spawned = -1;
    g.since_move = 1000;
    set_phase(g, Phase::Play);
}

// Runs when the slide lands: merge bursts, shake on big merges, then win and game-over checks.
void on_landed(lam::Ctx &ctx, Game &g) {
    const Fx *fx = ctx.try_res<Fx>();
    int biggest = 0;
    for (int i = 0; i < kCells; ++i) {
        if (g.src_b[i] < 0) continue;
        biggest = std::max(biggest, g.cells[i]);
        if (fx != nullptr) {
            const auto count = static_cast<std::uint16_t>(6 + 2 * static_cast<int>(std::log2(g.cells[i])));
            ctx.emit(lam::BurstAt{.config = fx->pop, .position = cell_center(i), .count = count});
        }
    }
    if (biggest >= 128) ctx.emit(lam::ShakeAt{.trauma = biggest >= 1024 ? 0.45f : 0.25f});
    if (!g.keep_going && max_tile(g.cells) >= kWinValue) {
        set_phase(g, Phase::Won);
        ctx.emit(lam::ShakeAt{.trauma = 0.7f});
        if (fx != nullptr)
            for (int i = 0; i < 4; ++i)
                ctx.emit(lam::BurstAt{.config = fx->confetti, .position = cell_center(i * 5), .count = 40});
    } else if (!can_move(g.cells)) {
        set_phase(g, Phase::Over);
        ctx.emit(lam::ShakeAt{.trauma = 0.5f});
    }
}

void play(lam::Ctx &ctx) {
    Game &g = ctx.local<Game>();
    const Controls &c = ctx.res<Controls>();
    const lam::InputFrame &in = lam::input_frame(ctx);
    g.since_move = std::min(g.since_move + 1, 1000);
    g.phase_ticks = std::min(g.phase_ticks + 1, 100000);
    if (g.since_move == kSlideTicks && g.phase == Phase::Play) on_landed(ctx, g);

    // A long drag is a swipe, a short one a click.
    int swipe = -1;
    bool clicked = false;
    const lam::Vec2 p = in.pointer_virtual;
    if (in.is_pressed(c.click)) {
        g.dragging = true;
        g.drag_x = static_cast<std::int32_t>(p.x);
        g.drag_y = static_cast<std::int32_t>(p.y);
    }
    if (g.dragging && in.is_released(c.click)) {
        g.dragging = false;
        const float dx = p.x - static_cast<float>(g.drag_x), dy = p.y - static_cast<float>(g.drag_y);
        if (std::abs(dx) < 16 && std::abs(dy) < 16) clicked = true;
        else if (std::abs(dx) > std::abs(dy)) swipe = dx > 0 ? kRight : kLeft;
        else swipe = dy > 0 ? kDown : kUp;
    }
    const lam::Vec2 at{static_cast<float>(g.drag_x), static_cast<float>(g.drag_y)};
    const bool click_new = clicked && kNewButton.contains(at);
    const bool click_undo = clicked && kUndoButton.contains(at);

    switch (g.phase) {
    case Phase::Title:
        if (in.is_pressed(c.confirm) || clicked) new_game(g, ctx.rng("spawn"_id));
        return;
    case Phase::Won:
        if (in.is_pressed(c.confirm) || (clicked && !click_new)) {
            g.keep_going = true;
            set_phase(g, Phase::Play);
        } else if (in.is_pressed(c.restart) || click_new) {
            new_game(g, ctx.rng("spawn"_id));
        }
        return;
    case Phase::Over:
        if (in.is_pressed(c.undo) || click_undo) undo(g);
        else if (in.is_pressed(c.confirm) || in.is_pressed(c.restart) || clicked) new_game(g, ctx.rng("spawn"_id));
        return;
    case Phase::Play: break;
    }
    if (in.is_pressed(c.restart) || click_new) return new_game(g, ctx.rng("spawn"_id));
    if (in.is_pressed(c.undo) || click_undo) return undo(g);
    if (g.since_move < kSlideTicks) {                           // let the slide land first
        if (in.is_repeated(c.up) || in.is_repeated(c.right) || in.is_repeated(c.down) || in.is_repeated(c.left) ||
            swipe >= 0) {
            g.since_move = kSlideTicks;
            on_landed(ctx, g);
            if (g.phase != Phase::Play) return;
        }
    }
    const lam::ButtonAction dirs[4] = {c.up, c.right, c.down, c.left};
    for (int d = 0; d < 4; ++d)
        if (in.is_repeated(dirs[d])) swipe = d;
    if (swipe >= 0) do_move(ctx, g, swipe);
}

void press(lam::InputFrame &f, lam::ButtonAction a) {
    if (a) f.press(a);
}

}  // namespace

void init_game(lam::World &w) {
    lam::Resources &r = w.resources();
    if (r.has<Game>()) return;
    Game &g = r.insert<Game>();
    g.src_a.fill(-1);
    g.src_b.fill(-1);
    w.snapshot_resource<Game>();
}

void Bot::fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) {
    const Game *g = ctx.world().resources().try_get<Game>();
    if (g == nullptr) return;
    lam::InputFrame &f = frames[0];
    switch (g->phase) {
    case Phase::Title:
        press(f, c_.confirm);
        return;
    case Phase::Won:
        if (g->phase_ticks > 60) press(f, c_.confirm);
        return;
    case Phase::Over:
        if (g->phase_ticks > 90) press(f, c_.restart);
        return;
    case Phase::Play: break;
    }
    if (g->since_move < kSlideTicks + 3) return;                // let the tween show
    const int d = best_dir(g->cells);
    const lam::ButtonAction dirs[4] = {c_.up, c_.right, c_.down, c_.left};
    if (d >= 0) press(f, dirs[d]);
}

void RulesPlugin::build(lam::App &app) {
    app.require_plugin("lam.input", name());
    app.add_event<lam::BurstAt>();
    app.add_event<lam::ShakeAt>();
    lam::ActionMap &map = app.res<lam::ActionMap>();
    const auto key = [](lam::Key k) { return lam::ActionMap::key(k); };
    const auto pad = [](lam::PadButton b) { return lam::ActionMap::pad(b); };
    Controls c{.up = map.button("up", {key(lam::Key::Up), key(lam::Key::W), pad(lam::PadButton::DUp)}),
               .right = map.button("right", {key(lam::Key::Right), key(lam::Key::D), pad(lam::PadButton::DRight)}),
               .down = map.button("down", {key(lam::Key::Down), key(lam::Key::S), pad(lam::PadButton::DDown)}),
               .left = map.button("left", {key(lam::Key::Left), key(lam::Key::A), pad(lam::PadButton::DLeft)}),
               .undo = map.button("undo", {key(lam::Key::Z), key(lam::Key::U), key(lam::Key::Backspace),
                                           pad(lam::PadButton::West)}),
               .restart = map.button("restart", {key(lam::Key::R), key(lam::Key::N), pad(lam::PadButton::Select)}),
               .confirm = map.button("confirm", {key(lam::Key::Enter), key(lam::Key::Space), pad(lam::PadButton::South)}),
               .click = map.button("click", {lam::ActionMap::mouse(lam::MouseButton::Left)})};
    app.insert_resource<Controls>(c);
    app.add_system({.stage = lam::Stage::Simulate, .fn = play, .name = "g2048.play", .run_if = &lam::has_local<Game>});
}

}  // namespace g2048
