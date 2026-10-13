// Asteroids rules.
//   Input        control   title/over -> start; ship turn, thrust, fire
//   Simulate  0  move      integrate bodies, wrap edges, drag, timers, exhaust puffs
//   Simulate 10  collide   bullets vs rocks (split), ship vs rocks (death, lives)
//   Simulate 20  waves     attract rocks on the title, respawn, next wave
#include <asteroids/game.h>

#include <algorithm>
#include <lam/ecs/commands.h>
#include <lam/ecs/lifetime.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/keys.h>
#include <lam/math/scalar.h>
#include <lam/render/camera_math.h>

namespace ast {
using namespace lam;
using namespace lam::literals;

namespace {
constexpr float kTurnRate = 4.6f;                               // rad/s
constexpr float kThrust = 230.0f;                               // px/s^2
constexpr float kMaxSpeed = 250.0f;
constexpr float kDrag = 0.993f;                                 // per tick
constexpr float kBulletSpeed = 380.0f;
constexpr std::uint32_t kBulletTicks = 52;
constexpr std::uint16_t kFireTicks = 9;
constexpr std::size_t kMaxBullets = 6;
constexpr std::uint16_t kInvulnTicks = 150;
constexpr Vec2 kCenter{kW / 2, kH / 2};

float wrapf(float v, float size) {
    while (v < 0) v += size;
    while (v >= size) v -= size;
    return v;
}

void spawn_ship(Commands &cmd) {
    const Entity e = cmd.spawn();
    cmd.add(e, Transform{.position = kCenter, .rotation = -kPi / 2});
    cmd.add(e, Body{.vel = {}, .radius = 8});
    cmd.add(e, Ship{.invuln = kInvulnTicks});
}

void spawn_rock(Ctx &ctx, Vec2 at, std::int32_t size, std::int32_t wave) {
    Rng &rng = ctx.rng("ast.rocks"_id);
    const float lo = size >= 3 ? 22.0f : size == 2 ? 40.0f : 65.0f;
    const float speed = (lo + rng.unit() * lo * 0.9f) * (1.0f + 0.07f * static_cast<float>(std::min(wave, 10)));
    Commands &cmd = ctx.commands();
    const Entity e = cmd.spawn();
    cmd.add(e, Transform{.position = at, .rotation = rng.unit() * kTau});
    cmd.add(e, Body{.vel = from_angle(rng.unit() * kTau, speed), .radius = rock_radius(size)});
    cmd.add(e, Rock{.size = size, .shape = rng.next_u32() | 1u});
}

void add_score(Run &run, std::int32_t points) {
    run.score += points;
    run.best = std::max(run.best, run.score);
    if (run.score >= run.next_life) {
        ++run.lives;
        run.next_life += 10000;
    }
}

// Debris, points and two smaller rocks. Sets rock.size to 0 to mark it dead this tick.
void break_rock(Ctx &ctx, Entity e, Vec2 at, Rock &rock, bool scored) {
    Run &run = ctx.local<Run>();
    const std::int32_t size = rock.size;
    rock.size = 0;
    ctx.commands().destroy(e);
    if (scored) {
        add_score(run, size >= 3 ? 20 : size == 2 ? 50 : 100);
        ++run.kills;
    }
    if (const Fx *fx = ctx.try_res<Fx>(); fx != nullptr && fx->spark)
        ctx.emit(BurstAt{.config = fx->spark, .position = at, .count = static_cast<std::uint16_t>(8 * size + 6)});
    if (size >= 3) ctx.emit(ShakeAt{.trauma = 0.35f});
    if (size > 1)
        for (int i = 0; i < 2; ++i) spawn_rock(ctx, at, size - 1, run.wave);
}

void new_game(Ctx &ctx) {
    World &w = ctx.world();
    Commands &cmd = ctx.commands();
    for (auto [e, r] : w.view<Rock>()) cmd.destroy(e);
    for (auto [e, t] : w.view<Transform>(With<Bullet>{})) cmd.destroy(e);
    for (auto [e, s] : w.view<Ship>()) cmd.destroy(e);
    Run &run = ctx.local<Run>();
    run = Run{.phase = kPlay, .best = run.best, .kills = run.kills, .deaths = run.deaths, .games = run.games + 1};
    spawn_ship(cmd);
}

void control(Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    if (run.phase != kPlay) {
        if (run.phase == kOver) ++run.over_ticks;
        const bool ready = run.phase == kTitle || run.over_ticks > 45;
        if (ready && (in.is_pressed(c.start) || in.is_pressed(c.fire))) new_game(ctx);
        return;
    }
    const float dt = ctx.time().tick_dt;
    std::size_t bullets = w.count<Bullet>();
    for (auto [e, t, b, s] : w.view<Transform, Body, Ship>()) {
        const float turn = (in.is_held(c.right) ? 1.0f : 0.0f) - (in.is_held(c.left) ? 1.0f : 0.0f);
        t.rotation += turn * kTurnRate * dt;
        if (t.rotation > kPi) t.rotation -= kTau;
        if (t.rotation < -kPi) t.rotation += kTau;
        s.thrusting = in.is_held(c.thrust);
        if (s.thrusting) b.vel += from_angle(t.rotation, kThrust * dt);
        if (s.cooldown > 0) --s.cooldown;
        if (in.is_held(c.fire) && s.cooldown == 0 && bullets < kMaxBullets) {
            s.cooldown = kFireTicks;
            ++bullets;
            const Vec2 dir = from_angle(t.rotation);
            Commands &cmd = ctx.commands();
            const Entity bullet = cmd.spawn();
            cmd.add(bullet, Transform{.position = t.position + dir * 11.0f});
            cmd.add(bullet, Body{.vel = b.vel + dir * kBulletSpeed, .radius = 2});
            cmd.add(bullet, Bullet{});
            cmd.add(bullet, Lifetime{.ticks = kBulletTicks});
        }
    }
}

void move(Ctx &ctx) {
    World &w = ctx.world();
    const float dt = ctx.time().tick_dt;
    for (auto [e, t, b] : w.view<Transform, Body>()) {
        if (Ship *s = w.try_get<Ship>(e)) {
            b.vel = clamp_length(b.vel * kDrag, kMaxSpeed);
            if (s->invuln > 0) --s->invuln;
            const Fx *fx = ctx.try_res<Fx>();
            if (s->thrusting && fx != nullptr && fx->exhaust && w.tick() % 2 == 0)
                ctx.emit(BurstAt{.config = fx->exhaust, .position = t.position - from_angle(t.rotation, 9.0f), .count = 2});
        }
        if (const Rock *r = w.try_get<Rock>(e)) t.rotation += (static_cast<float>(r->shape % 7) - 3.0f) * 0.25f * dt;
        const Vec2 before = t.position + b.vel * dt;
        const Vec2 after{wrapf(before.x, kW), wrapf(before.y, kH)};
        t.position = after;
        if (PrevTransform *prev = w.try_get<PrevTransform>(e)) prev->position += after - before;   // don't lerp across the wrap
    }
}

void collide(Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();
    for (auto [be, bt, bb] : w.view<Transform, Body>(With<Bullet>{})) {
        for (auto [re, rt, rb, rock] : w.view<Transform, Body, Rock>()) {
            if (rock.size == 0) continue;
            const float reach = rb.radius + bb.radius;
            if (length_sq(wrap_delta(bt.position, rt.position)) > reach * reach) continue;
            ctx.commands().destroy(be);
            break_rock(ctx, re, rt.position, rock, run.phase == kPlay);
            break;
        }
    }
    for (auto [se, st, sb, ship] : w.view<Transform, Body, Ship>()) {
        if (ship.invuln > 0 || run.phase != kPlay) continue;
        for (auto [re, rt, rb, rock] : w.view<Transform, Body, Rock>()) {
            if (rock.size == 0) continue;
            const float reach = rb.radius + sb.radius * 0.75f;
            if (length_sq(wrap_delta(st.position, rt.position)) > reach * reach) continue;
            break_rock(ctx, re, rt.position, rock, true);
            ctx.commands().destroy(se);
            if (const Fx *fx = ctx.try_res<Fx>(); fx != nullptr && fx->boom)
                ctx.emit(BurstAt{.config = fx->boom, .position = st.position, .count = 70});
            ctx.emit(ShakeAt{.trauma = 0.8f});
            ++run.deaths;
            if (--run.lives > 0) {
                run.respawn = 100;
            } else {
                run.phase = kOver;
                run.over_ticks = 0;
            }
            break;
        }
    }
}

void spawn_wave(Ctx &ctx, Run &run, int count, std::int32_t size) {
    Rng &rng = ctx.rng("ast.wave"_id);
    for (int i = 0; i < count; ++i) {
        Vec2 at{};
        for (int tries = 0; tries < 8; ++tries) {                // keep clear of the centre spawn
            at = {rng.unit() * kW, rng.unit() * kH};
            if (distance_sq(at, kCenter) > 150.0f * 150.0f) break;
        }
        spawn_rock(ctx, at, size == 0 ? 1 + static_cast<std::int32_t>(rng.below(3)) : size, run.wave);
    }
}

void waves(Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();
    const std::size_t rocks = w.count<Rock>();
    if (run.phase != kPlay && rocks == 0) spawn_wave(ctx, run, 7, 0);   // attract mode
    if (run.phase != kPlay) return;
    if (run.respawn > 0 && --run.respawn == 0) spawn_ship(ctx.commands());
    if (rocks > 0) return;
    if (run.wave_delay == 0) {
        run.wave_delay = run.wave == 0 ? 30 : 110;
    } else if (--run.wave_delay == 0) {
        ++run.wave;
        spawn_wave(ctx, run, std::min(3 + run.wave, 11), 3);
    }
}

void rule(App &app, Stage stage, SystemFn fn, std::string_view name, int order) {
    app.add_system({.stage = stage, .fn = fn, .name = name, .order = order, .run_if = &has_local<Run>});
}
}

Vec2 wrap_delta(Vec2 a, Vec2 b) {
    Vec2 d = b - a;
    if (d.x > kW / 2) d.x -= kW;
    if (d.x < -kW / 2) d.x += kW;
    if (d.y > kH / 2) d.y -= kH;
    if (d.y < -kH / 2) d.y += kH;
    return d;
}

Entity find_ship(World &w) {
    for (auto [e, s] : w.view<Ship>()) return e;
    return {};
}

void start_world(World &w) {
    w.resources().insert<Run>();
    w.snapshot_resource<Run>();
}

void RulesPlugin::build(App &app) {
    app.require_plugin("lam.input", name());
    app.register_component<Body>().register_component<Ship>();
    app.register_component<Rock>().register_component<Bullet>();
    app.add_event<BurstAt>();
    app.add_event<ShakeAt>();
    ActionMap &map = app.res<ActionMap>();
    const auto k = [](Key key) { return ActionMap::key(key); };
    app.insert_resource<Controls>(Controls{
        .left = map.button("left", {k(Key::Left), k(Key::A), ActionMap::pad(PadButton::DLeft)}),
        .right = map.button("right", {k(Key::Right), k(Key::D), ActionMap::pad(PadButton::DRight)}),
        .thrust = map.button("thrust", {k(Key::Up), k(Key::W), ActionMap::mouse(MouseButton::Right),
                                        ActionMap::pad(PadButton::RT)}),
        .fire = map.button("fire", {k(Key::Space), k(Key::J), ActionMap::mouse(MouseButton::Left),
                                    ActionMap::pad(PadButton::South)}),
        .start = map.button("start", {k(Key::Enter), k(Key::KpEnter), ActionMap::pad(PadButton::Start)})});
    rule(app, Stage::Input, control, "asteroids.control", 0);
    rule(app, Stage::Simulate, move, "asteroids.move", 0);
    rule(app, Stage::Simulate, collide, "asteroids.collide", 10);
    rule(app, Stage::Simulate, waves, "asteroids.waves", 20);
}
}
