// Missile Command rules.
//   Input        control    phases, crosshair, launches
//   Simulate     step       spawning, movement, splitting, impacts, blast kills
//   PostPhysics  wave_end   wave clear, bonus, next wave (runs after this tick's commands apply)
#include <game/game.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <lam/ecs/commands.h>
#include <lam/ecs/world.h>
#include <lam/input/keys.h>
#include <lam/render/camera_math.h>

namespace game {
using namespace lam;
using namespace lam::literals;

float blast_radius(const Blast &b) {
    const float a = b.age;
    const float m = b.hostile ? kBlastMax * 0.8f : kBlastMax;
    if (a < kBlastGrow) return m * a / kBlastGrow;
    if (a < kBlastGrow + kBlastHold) return m;
    return std::max(0.0f, m * (1.0f - (a - kBlastGrow - kBlastHold) / kBlastShrink));
}
std::int32_t &ammo(Run &r, int base) { return base == 0 ? r.ammo0 : base == 1 ? r.ammo1 : r.ammo2; }
std::int32_t ammo(const Run &r, int base) { return base == 0 ? r.ammo0 : base == 1 ? r.ammo1 : r.ammo2; }
int city_count(const Run &r) { return std::popcount(r.cities & 0x3Fu); }

namespace {
constexpr float kCounterSpeed = 6;                               // px per tick
constexpr std::uint16_t kBlastLife = kBlastGrow + kBlastHold + kBlastShrink;

void begin_wave(Run &r) {
    ++r.wave;
    r.ammo0 = r.ammo1 = r.ammo2 = kAmmoPerBase;
    r.to_spawn = std::min(8 + 2 * r.wave, 30);
    r.timer = 60;
    r.phase = kPlay;
}

void new_game(Ctx &ctx) {
    World &w = ctx.world();
    for (auto [e, x] : w.view<Enemy>()) ctx.commands().destroy(e);
    for (auto [e, x] : w.view<Counter>()) ctx.commands().destroy(e);
    for (auto [e, x] : w.view<Blast>()) ctx.commands().destroy(e);
    Run &r = ctx.local<Run>();
    r = Run{.best = r.best, .cross = r.cross, .last_pointer = r.last_pointer};
    begin_wave(r);
}

Vec2 target_point(std::uint32_t i) {                             // 0..5 cities, 6..8 bases
    return i < kCities ? Vec2{kCityX[i], kGround} : Vec2{kBaseX[i - kCities], kGround};
}

void spawn_enemy(Ctx &ctx, Vec2 from, bool may_split) {
    Run &r = ctx.local<Run>();
    Rng &rng = ctx.rng("missile.enemy"_id);
    const Vec2 to = target_point(rng.below(kCities + kBases));
    const float speed = std::min(1.3f, 0.30f + 0.06f * static_cast<float>(r.wave));
    float split = 0;
    if (may_split && r.wave >= 2 && rng.unit() < 0.2f + 0.04f * static_cast<float>(r.wave))
        split = static_cast<float>(rng.between(60, 150));
    const Entity e = ctx.commands().spawn();
    ctx.commands().add<Enemy>(e, Enemy{.from = from, .pos = from, .to = to, .speed = speed, .split_y = split});
}

void boom(Ctx &ctx, Vec2 at, std::uint16_t count, bool debris) {
    if (const Fx *fx = ctx.try_res<Fx>(); fx != nullptr)
        ctx.emit(BurstAt{.config = debris ? fx->debris : fx->boom, .position = at, .count = count});
}

void control(Ctx &ctx) {
    Run &r = ctx.local<Run>();
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    // Crosshair follows the mouse when it moves, otherwise the keys.
    if (in.pointer_valid && distance_sq(in.pointer_world, r.last_pointer) > 0.01f) {
        r.cross = in.pointer_world;
        r.last_pointer = in.pointer_world;
    }
    r.cross += in.axis2(c.move) * 4.0f;
    r.cross = {std::clamp(r.cross.x, 4.0f, kW - 4), std::clamp(r.cross.y, 12.0f, kGround - 28)};
    const bool any = in.is_pressed(c.fire) || in.is_pressed(c.b1) || in.is_pressed(c.b2) || in.is_pressed(c.b3);
    if (r.phase == kTitle || r.phase == kOver) {
        if (r.timer > 0) --r.timer;
        if (any && r.timer == 0) new_game(ctx);
        return;
    }
    if (r.phase != kPlay || !any) return;
    int base = in.is_pressed(c.b1) ? 0 : in.is_pressed(c.b2) ? 1 : in.is_pressed(c.b3) ? 2 : -1;
    if (base < 0) {                                              // nearest base with ammo
        float best = 1e9f;
        for (int b = 0; b < kBases; ++b)
            if (ammo(r, b) > 0 && std::fabs(kBaseX[static_cast<std::size_t>(b)] - r.cross.x) < best) {
                best = std::fabs(kBaseX[static_cast<std::size_t>(b)] - r.cross.x);
                base = b;
            }
    }
    if (base < 0 || ammo(r, base) <= 0) return;
    --ammo(r, base);
    const Vec2 from{kBaseX[static_cast<std::size_t>(base)], kBaseY - 6};
    const Entity e = ctx.commands().spawn();
    ctx.commands().add<Counter>(e, Counter{.from = from, .pos = from, .to = r.cross});
}

void step(Ctx &ctx) {
    World &w = ctx.world();
    Run &r = ctx.local<Run>();
    Commands &cmd = ctx.commands();
    if (r.phase == kBonus && r.timer > 0 && --r.timer == 0) begin_wave(r);
    if (r.phase == kPlay && r.to_spawn > 0 && (r.timer == 0 || --r.timer == 0)) {
        Rng &rng = ctx.rng("missile.wave"_id);
        const int burst = std::min(r.to_spawn, 1 + static_cast<int>(rng.below(r.wave >= 3 ? 3u : 2u)));
        for (int i = 0; i < burst; ++i)
            spawn_enemy(ctx, {static_cast<float>(rng.between(8, static_cast<int>(kW) - 8)), -4}, true);
        r.to_spawn -= burst;
        r.timer = static_cast<std::uint32_t>(rng.between(std::max(25, 90 - r.wave * 7), std::max(50, 150 - r.wave * 9)));
    }
    for (auto [e, c] : w.view<Counter>()) {
        c.pos += normalize_or(c.to - c.from) * kCounterSpeed;
        if (distance_sq(c.pos, c.from) >= distance_sq(c.to, c.from)) {
            cmd.destroy(e);
            const Entity b = cmd.spawn();
            cmd.add<Blast>(b, Blast{.pos = c.to});
        }
    }
    for (auto [e, b] : w.view<Blast>())
        if (++b.age >= kBlastLife) cmd.destroy(e);
    for (auto [e, m] : w.view<Enemy>()) {
        m.pos += normalize_or(m.to - m.from) * m.speed;
        bool dead = false;
        for (auto [be, b] : w.view<Blast>()) {
            const float rad = blast_radius(b);
            if (b.hostile || b.age >= kBlastLife || distance_sq(m.pos, b.pos) > rad * rad) continue;
            cmd.destroy(e);
            if (r.phase == kPlay) { r.score += 25; ++r.kills; }
            boom(ctx, m.pos, 14, false);
            dead = true;
            break;
        }
        if (dead) continue;
        if (m.split_y > 0 && m.pos.y >= m.split_y) {             // MIRV: split off two more
            m.split_y = 0;
            spawn_enemy(ctx, m.pos, false);
            spawn_enemy(ctx, m.pos, false);
            boom(ctx, m.pos, 6, false);
        }
        if (m.pos.y < m.to.y) continue;
        cmd.destroy(e);                                          // impact
        const Entity hb = cmd.spawn();
        cmd.add<Blast>(hb, Blast{.pos = m.to, .hostile = true});
        boom(ctx, m.to, 24, true);
        for (int i = 0; i < kCities; ++i)
            if (std::fabs(kCityX[static_cast<std::size_t>(i)] - m.to.x) < 1 && (r.cities >> i & 1u)) {
                r.cities &= ~(1u << i);
                ctx.emit(ShakeAt{.trauma = 0.55f});
            }
        for (int b = 0; b < kBases; ++b)
            if (std::fabs(kBaseX[static_cast<std::size_t>(b)] - m.to.x) < 1 && ammo(r, b) > 0) {
                ammo(r, b) = 0;
                ctx.emit(ShakeAt{.trauma = 0.45f});
            }
        if (r.phase == kPlay && r.cities == 0) {
            r.phase = kOver;
            r.timer = 90;
            r.best = std::max(r.best, r.score);
            ctx.emit(ShakeAt{.trauma = 1.0f});
        }
    }
}

void wave_end(Ctx &ctx) {
    World &w = ctx.world();
    Run &r = ctx.local<Run>();
    if (r.phase != kPlay || r.to_spawn > 0 || w.count<Enemy>() > 0 || w.count<Counter>() > 0 || w.count<Blast>() > 0)
        return;
    r.bonus = city_count(r) * 100 + (r.ammo0 + r.ammo1 + r.ammo2) * 5;
    r.score += r.bonus;
    r.best = std::max(r.best, r.score);
    r.phase = kBonus;
    r.timer = 150;
}

void rule(App &app, Stage s, SystemFn fn, std::string_view n) {
    app.add_system({.stage = s, .fn = fn, .name = n, .run_if = &has_local<Run>});
}
}

void start(World &w) {
    w.resources().insert<Run>();
    w.snapshot_resource<Run>();
}

void RulesPlugin::build(App &app) {
    app.require_plugin("lam.input", name());
    app.register_component<Enemy>().register_component<Counter>().register_component<Blast>();
    app.add_event<BurstAt>();
    app.add_event<ShakeAt>();
    ActionMap &map = app.res<ActionMap>();
    const auto key = [](Key k) { return ActionMap::key(k); };
    app.insert_resource<Controls>(Controls{
        .fire = map.button("fire", {ActionMap::mouse(MouseButton::Left), key(Key::Space), key(Key::Enter)}),
        .b1 = map.button("base1", {key(Key::Num1), key(Key::Z)}),
        .b2 = map.button("base2", {key(Key::Num2), key(Key::X)}),
        .b3 = map.button("base3", {key(Key::Num3), key(Key::C)}),
        .move = map.axis2("cross", {{key(Key::W), key(Key::S), key(Key::A), key(Key::D)},
                                    {key(Key::Up), key(Key::Down), key(Key::Left), key(Key::Right)}})});
    rule(app, Stage::Input, control, "missile.control");
    rule(app, Stage::Simulate, step, "missile.step");
    rule(app, Stage::PostPhysics, wave_end, "missile.wave_end");
}
}
