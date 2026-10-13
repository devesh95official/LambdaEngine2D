#include "dino/rules.h"

#include <algorithm>
#include <lam/core/id.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
#include <lam/math/rng.h>
#include <lam/particles/components.h>
#include <lam/render/camera_math.h>

namespace dino {
namespace {

constexpr lam::StrId kSpawnStream = lam::str_id("dino.spawn");

void burst(const lam::Ctx &ctx, lam::AssetId<lam::EmitterConfig> Fx::*which, lam::Vec2 at, std::uint16_t count) {
    if (const Fx *fx = ctx.try_res<Fx>()) ctx.emit(lam::BurstAt{.config = fx->*which, .position = at, .count = count});
}

void start_run(const lam::Ctx &ctx, DinoState &s) {
    lam::World &w = ctx.world();
    for (auto [e, ob] : w.view<Obstacle>()) ctx.commands().destroy(e);   // deferred to the end of the stage
    const std::int32_t best = s.best, last = s.last_score, runs = s.runs + 1;
    const lam::Entity runner = s.runner;
    s = DinoState{};
    s.phase = Phase::Play;
    s.best = best;
    s.last_score = last;
    s.runs = runs;
    s.runner = runner;
    w.get<Runner>(runner) = Runner{};
    w.teleport(runner, {kTune.runner_x, kTune.ground_y});
}

Obstacle make_obstacle(Kind k, std::int32_t variant) {
    switch (k) {
    case Kind::SmallCactus: return {k, 10, 22, variant};
    case Kind::BigCactus: return {k, 14, 32, variant};
    case Kind::Cluster: return {k, 28, 24, variant};
    default: return {k, 22, 12, variant};
    }
}

float bird_lift(Kind k) { return k == Kind::BirdLow ? 4.0f : k == Kind::BirdMid ? 20.0f : 38.0f; }

void spawn_obstacle(lam::World &w, DinoState &s) {
    lam::Rng &rng = w.rng(kSpawnStream);
    const std::uint32_t roll = rng.below(100);
    Kind k = Kind::SmallCactus;
    if (s.score >= kTune.birds_from && roll < 28) {
        k = static_cast<Kind>(static_cast<int>(Kind::BirdLow) + static_cast<int>(rng.below(3)));
    } else {
        const std::uint32_t c = rng.below(10);
        k = c < 5 ? Kind::SmallCactus : c < 8 ? Kind::BigCactus : Kind::Cluster;
    }
    const Obstacle ob = make_obstacle(k, static_cast<std::int32_t>(rng.below(4)));
    const bool bird = k >= Kind::BirdLow;
    const lam::Entity e = w.create();
    w.add<lam::Transform>(e, lam::Transform{.position = {kVirtualW + 40, kTune.ground_y - (bird ? bird_lift(k) : 0)}});
    w.add<Obstacle>(e, ob);
    const float min_gap = s.speed * 34 + 70;
    s.next_spawn = s.distance + min_gap + rng.range(0, 240);
}

void control_system(lam::Ctx &ctx) {
    const Controls *c = ctx.try_res<Controls>();
    if (c == nullptr) return;
    DinoState &s = ctx.local<DinoState>();
    const lam::InputFrame &in = lam::input_frame(ctx);
    const bool go = in.is_pressed(c->jump) || in.is_pressed(c->start);
    if ((s.phase == Phase::Title && go) || (s.phase == Phase::Over && s.ticks_in_phase >= kTune.restart_delay && go)) {
        start_run(ctx, s);
        return;
    }
    if (s.phase != Phase::Play) return;
    Runner &r = ctx.world().get<Runner>(s.runner);
    const bool duck = in.is_held(c->duck);
    r.jump_held = in.is_held(c->jump);
    if (r.grounded && in.is_pressed(c->jump)) {
        r.vy = -kTune.jump_v;
        r.grounded = false;
        r.air_ticks = 0;
    }
    r.ducking = r.grounded && duck;
    r.fast_fall = !r.grounded && duck;
}

void step_system(lam::Ctx &ctx) {
    DinoState &s = ctx.local<DinoState>();
    ++s.ticks_in_phase;
    if (s.phase != Phase::Play) return;
    lam::World &w = ctx.world();
    ++s.play_ticks;
    s.speed = std::min(kTune.max_speed, s.speed + kTune.accel);
    s.distance += s.speed;
    s.score = static_cast<std::int32_t>(s.distance * 0.04f);

    Runner &r = w.get<Runner>(s.runner);
    lam::Transform &tf = w.get<lam::Transform>(s.runner);
    if (!r.grounded) {
        if (!r.jump_held && r.vy < -kTune.cut_v) r.vy = -kTune.cut_v;   // short hop
        r.vy += kTune.gravity + (r.fast_fall ? kTune.fast_fall : 0.0f);
        tf.position.y += r.vy;
        ++r.air_ticks;
        if (tf.position.y >= kTune.ground_y) {
            tf.position.y = kTune.ground_y;
            r.vy = 0;
            r.grounded = true;
            r.fast_fall = false;
            if (r.air_ticks > 4) burst(ctx, &Fx::dust, tf.position, 12);
        }
    } else if (s.play_ticks % 8 == 0) {
        burst(ctx, &Fx::kick, tf.position + lam::Vec2{-6, 0}, 2);
    }

    const lam::Rect me = runner_box(tf.position, r).expanded(-2);
    bool hit = false;
    for (auto [e, ob, otf] : w.view<Obstacle, lam::Transform>()) {
        otf.position.x -= s.speed;
        if (otf.position.x + ob.w < -40) {
            ctx.commands().destroy(e);
            continue;
        }
        if (me.overlaps(obstacle_box(otf.position, ob).expanded(-2))) hit = true;
    }
    if (hit) {
        s.phase = Phase::Over;
        s.ticks_in_phase = 0;
        s.last_score = s.score;
        s.best = std::max(s.best, s.score);
        burst(ctx, &Fx::crash, me.center(), 40);
        ctx.emit(lam::ShakeAt{.trauma = 0.75f});
        return;
    }
    if (s.distance >= s.next_spawn) spawn_obstacle(w, s);
}

} // namespace

lam::Rect runner_box(lam::Vec2 feet, const Runner &r) {
    if (r.ducking) return {feet.x - 11, feet.y - 13, 26, 13};
    return {feet.x - 9, feet.y - 24, 18, 24};
}

lam::Rect obstacle_box(lam::Vec2 bottom, const Obstacle &o) {
    return {bottom.x - o.w / 2, bottom.y - o.h, o.w, o.h};
}

void setup_world(lam::World &w) {
    w.resources().insert<DinoState>();
    w.snapshot_resource<DinoState>();
    DinoState &s = w.resources().get<DinoState>();
    s.runner = w.create();
    w.add<lam::Transform>(s.runner, lam::Transform{.position = {kTune.runner_x, kTune.ground_y}});
    w.add<Runner>(s.runner);
}

BotChoice bot_decide(lam::World &w) {
    BotChoice c;
    const DinoState *s = w.resources().try_get<DinoState>();
    if (s == nullptr) return c;
    if (s->phase == Phase::Title) return {.start = true};
    if (s->phase == Phase::Over) return {.start = s->ticks_in_phase >= kTune.restart_delay + 30};
    const Runner *r = w.try_get<Runner>(s->runner);
    const lam::Transform *tf = w.try_get<lam::Transform>(s->runner);
    if (r == nullptr || tf == nullptr) return c;
    const lam::Rect me = runner_box(tf->position, Runner{});
    float best_dx = 1e9f, ow = 0;
    Kind kind = Kind::SmallCactus;
    for (auto [e, ob, otf] : w.view<Obstacle, lam::Transform>()) {
        const lam::Rect b = obstacle_box(otf.position, ob);
        if (b.x + b.w < me.x) continue;                         // already passed
        const float dx = b.x - (me.x + me.w);
        if (dx < best_dx) best_dx = dx, ow = ob.w, kind = ob.kind;
    }
    // Stop reacting past a score that changes each run, so --bot also shows game over and restart.
    const bool tired = s->score >= 400 + 450 * ((s->runs + 3) % 4);
    if (best_dx < 1e8f && !tired) {
        if (kind == Kind::BirdMid) c.duck = best_dx < s->speed * 6 + 8;
        else if (kind != Kind::BirdHigh) c.jump = r->grounded && best_dx <= 16 * s->speed - (18 + ow) / 2;
    }
    if (!r->grounded && r->vy < 0 && r->jump_held) c.jump = true;   // hold for full height
    return c;
}

void DinoBot::fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) {
    lam::InputFrame &f = frames[0];
    f.clear_devices();                                          // ignore keyboard and mouse
    const BotChoice c = bot_decide(ctx.world());
    const auto apply = [&f](lam::ButtonAction a, bool want, bool &was) {
        if (want) was ? f.hold(a) : f.press(a);
        else if (was) f.release(a);
        was = want;
    };
    apply(controls.jump, c.jump, was_jump_);
    apply(controls.duck, c.duck, was_duck_);
    apply(controls.start, c.start, was_start_);
}

void RulesPlugin::build(lam::App &app) {
    app.register_component<Runner>();
    app.register_component<Obstacle>();
    app.add_event<lam::BurstAt>();                              // also registered here for headless tests
    app.add_event<lam::ShakeAt>();
    app.add_system({.stage = lam::Stage::Input, .fn = control_system, .name = "dino.control",
                    .run_if = lam::has_local<DinoState>});
    app.add_system({.stage = lam::Stage::Simulate, .fn = step_system, .name = "dino.step",
                    .run_if = lam::has_local<DinoState>});
}

} // namespace dino
