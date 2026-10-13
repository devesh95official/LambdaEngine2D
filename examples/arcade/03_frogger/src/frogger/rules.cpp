// Frogger rules.
//   Input     frog_input   start, restart, hops from keys or a click
//   Simulate  tick_rules   carry, move lanes, deaths, homes, timers, levels
#include <frogger/frogger.h>

#include <algorithm>
#include <cstdlib>
#include <lam/ecs/commands.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/keys.h>
#include <lam/render/camera_math.h>

namespace frogger {
using namespace lam;

namespace {
constexpr std::int32_t kSpan = (kW + 2 * kMargin) * kMilli;

struct Lane { int row; std::uint8_t kind; int count; int w; int speed; };
constexpr Lane kLanes[] = {
    {1, kLog, 3, 64, 550},    {2, kTurtle, 4, 32, -750}, {3, kLog, 2, 96, 900},
    {4, kLog, 3, 48, 450},    {5, kTurtle, 4, 48, -600}, {7, kTruck, 2, 32, -550},
    {8, kRacer, 2, 16, 1500}, {9, kCar, 3, 16, -800},    {10, kDozer, 3, 16, 500},
    {11, kCar, 3, 16, -650},
};

std::int32_t wrap(std::int32_t x) {
    std::int32_t v = (x + kMargin * kMilli) % kSpan;
    if (v < 0) v += kSpan;
    return v - kMargin * kMilli;
}

bool river(int row) { return row >= 1 && row <= 5; }
bool road(int row) { return row >= 7 && row <= 11; }

// The mover under a frog at cx in `row`, `t` ticks ahead. Null means water.
const Mover *support(World &w, const Run &run, int row, std::int32_t cx, int t) {
    for (auto [e, m] : w.view<Mover>()) {
        if (m.row != row) continue;
        const std::int32_t x = x_at(m, run.level, t);
        if (cx >= x + kMilli && cx <= x + (m.w - 1) * kMilli && turtle_phase(m, run.clock + t) != 2)
            return &m;
    }
    return nullptr;
}

bool hit_by_car(World &w, const Run &run, int row, std::int32_t cx, int t) {
    for (auto [e, m] : w.view<Mover>()) {
        if (m.row != row) continue;
        const std::int32_t x = x_at(m, run.level, t);
        if (cx + 5 * kMilli > x + kMilli && cx - 5 * kMilli < x + (m.w - 1) * kMilli) return true;
    }
    return false;
}

void reset_frog(Run &run) {
    run.row = run.from_row = run.best_row = kStartRow;
    run.cx = run.from_cx = kStartCx * kMilli;
    run.timer = kFrogTicks;
    run.hop = 0;
    run.facing = 0;
    run.death = 0;
}

void new_game(Run &run) {
    const std::int32_t best = run.best;
    const std::uint32_t deaths = run.deaths;
    run = Run{};
    run.best = best;
    run.deaths = deaths;
    run.state = kPlay;
    reset_frog(run);
}

Vec2 frog_px(const Run &run) {
    return {static_cast<float>(run.cx) / kMilli, static_cast<float>(kFieldY + run.row * kCell + kCell / 2)};
}

void kill(const Ctx &ctx, Run &run, std::uint8_t how) {
    run.state = kDying;
    run.state_timer = 45;
    run.death = how;
    --run.lives;
    ++run.deaths;
    if (const Fx *fx = ctx.try_res<Fx>())
        ctx.emit(BurstAt{.config = how == 2 ? fx->splash : fx->squash, .position = frog_px(run), .count = 24});
    ctx.emit(ShakeAt{.trauma = how == 1 ? 0.6f : 0.4f});
}

void frog_input(Ctx &ctx) {
    Run &run = ctx.local<Run>();
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    const bool any = in.is_pressed(c.start) || in.is_pressed(c.click) || in.is_pressed(c.up);
    if (run.state == kTitle || (run.state == kOver && run.state_timer > 40)) {
        if (any) new_game(run);
        return;
    }
    if (run.state != kPlay || run.hop > 0) return;
    int dir = -1;
    if (in.is_pressed(c.up)) dir = 0;
    else if (in.is_pressed(c.right)) dir = 1;
    else if (in.is_pressed(c.down)) dir = 2;
    else if (in.is_pressed(c.left)) dir = 3;
    else if (in.is_pressed(c.click) && in.pointer_valid) {     // hop toward the click
        const Vec2 d = in.pointer_world - frog_px(run);
        if (std::abs(d.x) > std::abs(d.y)) dir = d.x > 0 ? 1 : 3;
        else dir = d.y > 0 ? 2 : 0;
    }
    if (dir < 0) return;
    const int nrow = run.row + (dir == 0 ? -1 : dir == 2 ? 1 : 0);
    std::int32_t ncx = run.cx + (dir == 1 ? kCell * kMilli : dir == 3 ? -kCell * kMilli : 0);
    if (nrow < 0 || nrow > kStartRow) return;
    ncx = std::clamp(ncx, (kCell / 2) * kMilli, (kW - kCell / 2) * kMilli);
    run.from_row = run.row;
    run.from_cx = run.cx;
    run.row = nrow;
    run.cx = ncx;
    run.facing = static_cast<std::uint8_t>(dir);
    run.hop = kHopTicks;
    ++run.hops;
    if (nrow < run.best_row) {
        run.best_row = nrow;
        run.score += 10;
    }
}

void tick_rules(Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();
    ++run.clock;
    ++run.state_timer;
    if (run.state == kPlay) {
        if (run.hop > 0) --run.hop;
        if (river(run.row)) {                                   // ride, or fall in
            if (const Mover *m = support(w, run, run.row, run.cx, 0)) {
                run.cx += speed_of(*m, run.level);
                run.from_cx += speed_of(*m, run.level);
            } else {
                kill(ctx, run, 2);
            }
        }
    }
    for (auto [e, m] : w.view<Mover>()) m.x = wrap(m.x + speed_of(m, run.level));
    if (run.state == kPlay) {
        if (road(run.row) && hit_by_car(w, run, run.row, run.cx, 0)) kill(ctx, run, 1);
        else if (run.cx < 2 * kMilli || run.cx > (kW - 2) * kMilli) kill(ctx, run, 3);
        else if (--run.timer <= 0) kill(ctx, run, 3);
        else if (run.row == 0 && run.hop == 0) {                // landed in the home row
            int slot = -1;
            for (int k = 0; k < kHomes; ++k)
                if (!(run.homes & (1u << k)) && std::abs(run.cx - slot_cx(k) * kMilli) <= 8 * kMilli) slot = k;
            if (slot < 0) {
                kill(ctx, run, 1);
            } else {
                run.homes = static_cast<std::uint8_t>(run.homes | (1u << slot));
                run.score += 50 + 10 * (run.timer / 60);
                ++run.homed;
                const Vec2 at{static_cast<float>(slot_cx(slot)), static_cast<float>(kFieldY + kCell / 2)};
                const Fx *fx = ctx.try_res<Fx>();
                if (fx) ctx.emit(BurstAt{.config = fx->sparkle, .position = at, .count = 20});
                if (run.homes == (1u << kHomes) - 1) {          // all five: next level
                    run.homes = 0;
                    ++run.level;
                    run.score += 1000;
                    ctx.emit(ShakeAt{.trauma = 0.3f});
                    for (int k = 0; fx && k < kHomes; ++k)
                        ctx.emit(BurstAt{.config = fx->sparkle,
                                         .position = {static_cast<float>(slot_cx(k)), at.y}, .count = 30});
                }
                reset_frog(run);
            }
        }
    } else if (run.state == kDying && run.state_timer >= 45) {
        run.state_timer = 0;
        if (run.lives > 0) {
            run.state = kPlay;
            reset_frog(run);
        } else {
            run.state = kOver;
        }
    }
    run.best = std::max(run.best, run.score);
}

void rule(App &app, Stage stage, SystemFn fn, std::string_view name) {
    app.add_system({.stage = stage, .fn = fn, .name = name, .run_if = &has_local<Run>});
}
} // namespace

std::int32_t speed_of(const Mover &m, std::int32_t level) {
    const std::int32_t l = std::min(level, 9) - 1;
    return m.base_speed * (100 + 15 * l) / 100;
}

std::int32_t x_at(const Mover &m, std::int32_t level, std::int32_t t) {
    return wrap(m.x + speed_of(m, level) * t);
}

int turtle_phase(const Mover &m, std::uint32_t clock) {
    if (!m.diver) return 0;
    const std::uint32_t p = (clock + static_cast<std::uint32_t>(m.row) * 53u) % 280u;
    if (p < 190) return 0;
    if (p < 215) return 1;
    if (p < 250) return 2;
    return 1;
}

bool safe_for(World &w, const Run &run, int row, std::int32_t cx, int ticks) {
    if (row == 0) {
        for (int k = 0; k < kHomes; ++k)
            if (!(run.homes & (1u << k)) && std::abs(cx - slot_cx(k) * kMilli) <= 6 * kMilli) return true;
        return false;
    }
    if (road(row)) {
        for (int t = 0; t <= ticks; ++t)
            if (hit_by_car(w, run, row, cx, t)) return false;
        return true;
    }
    if (river(row)) {
        const Mover *m = support(w, run, row, cx, 0);
        if (m == nullptr) return false;
        const std::int32_t v = speed_of(*m, run.level);
        for (int t = 1; t <= ticks; ++t) {
            const std::int32_t c = cx + v * t;
            if (c < 4 * kMilli || c > (kW - 4) * kMilli || turtle_phase(*m, run.clock + static_cast<std::uint32_t>(t)) == 2)
                return false;
        }
        return true;
    }
    return true;                                                // median and start strip
}

void start_world(World &w) {
    w.resources().insert<Run>();
    w.snapshot_resource<Run>();
    for (const Lane &l : kLanes)
        for (int i = 0; i < l.count; ++i) {
            const Entity e = w.create();
            const std::int32_t x = (i * (kW + 2 * kMargin) / l.count + l.row * 37) * kMilli;
            w.add<Mover>(e, Mover{.row = l.row, .x = wrap(x), .w = l.w, .base_speed = l.speed,
                                  .kind = l.kind, .diver = l.kind == kTurtle && i == 0});
        }
}

void RulesPlugin::build(App &app) {
    app.require_plugin("lam.input", name());
    app.register_component<Mover>();
    app.add_event<BurstAt>();
    app.add_event<ShakeAt>();
    ActionMap &map = app.res<ActionMap>();
    const auto key = [](Key k) { return ActionMap::key(k); };
    app.insert_resource<Controls>(Controls{
        .up = map.button("up", {key(Key::Up), key(Key::W), ActionMap::pad(PadButton::DUp)}),
        .down = map.button("down", {key(Key::Down), key(Key::S), ActionMap::pad(PadButton::DDown)}),
        .left = map.button("left", {key(Key::Left), key(Key::A), ActionMap::pad(PadButton::DLeft)}),
        .right = map.button("right", {key(Key::Right), key(Key::D), ActionMap::pad(PadButton::DRight)}),
        .start = map.button("start", {key(Key::Space), key(Key::Enter), ActionMap::pad(PadButton::Start),
                                      ActionMap::pad(PadButton::South)}),
        .click = map.button("click", {ActionMap::mouse(MouseButton::Left)})});
    rule(app, Stage::Input, frog_input, "frogger.input");
    rule(app, Stage::Simulate, tick_rules, "frogger.tick");
}
} // namespace frogger
