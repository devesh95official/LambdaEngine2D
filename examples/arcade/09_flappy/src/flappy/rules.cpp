// Flappy rules and the --bot driver.
//   Input     flappy.controls  flap, start, restart
//   Simulate  flappy.step      bird, pipes, scoring, collisions
#include <algorithm>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/render/camera_math.h>
#include <flappy/flappy.h>

namespace flappy {
using namespace lam;
using namespace lam::literals;

namespace {
void burst(const Ctx &ctx, AssetId<EmitterConfig> Fx::*which, Vec2 at, std::uint16_t n) {
    if (const Fx *fx = ctx.try_res<Fx>(); fx != nullptr && fx->*which)
        ctx.emit(BurstAt{.config = fx->*which, .position = at, .count = n});
}

void flap(const Ctx &ctx, Run &run, Bird &bird, Vec2 at) {
    bird.vy = kFlapVy;
    ++run.flaps;
    burst(ctx, &Fx::puff, at + Vec2{-kBirdR, 2}, 4);
}

void reset(Ctx &ctx, Run &run) {
    for (auto [e, p] : ctx.world().view<Pipe>()) ctx.commands().destroy(e);
    for (auto [e, b] : ctx.world().view<Bird>()) {
        b.vy = 0;
        ctx.commands().teleport(e, {kBirdX, kStartY});
    }
    run.phase = kTitle;
    run.phase_ticks = 0;
    run.score = 0;
}

void controls(Ctx &ctx) {
    Run &run = ctx.local<Run>();
    if (!input_frame(ctx).is_pressed(ctx.res<Controls>().flap)) return;
    const Entity e = find_bird(ctx.world());
    if (!e) return;
    Bird &bird = ctx.world().get<Bird>(e);
    const Vec2 at = ctx.world().get<Transform>(e).position;
    switch (run.phase) {
    case kTitle:
        run.phase = kPlay;
        run.phase_ticks = 0;
        run.next_pipe = 50;
        ++run.games;
        flap(ctx, run, bird, at);
        break;
    case kPlay: flap(ctx, run, bird, at); break;
    default:
        if (run.phase_ticks >= kRestartDelay) reset(ctx, run);
        break;
    }
}

void die(const Ctx &ctx, Run &run, Bird &bird, Vec2 at) {
    run.phase = kOver;
    run.phase_ticks = 0;
    ++run.deaths;
    run.best = std::max(run.best, run.score);
    bird.vy = std::min(bird.vy, -2.5f);                         // a little hop before the fall
    burst(ctx, &Fx::feathers, at, 26);
    ctx.emit(ShakeAt{.trauma = 0.75f});
}

bool hits_pipe(Vec2 b, const Transform &t, const Pipe &p) {
    const float r = kBirdR - 1.0f;                              // slightly forgiving
    if (b.x + r < t.position.x - kPipeW * 0.5f || b.x - r > t.position.x + kPipeW * 0.5f) return false;
    return b.y - r < p.gap_y - kGap * 0.5f || b.y + r > p.gap_y + kGap * 0.5f;
}

void step(Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();
    ++run.phase_ticks;
    if (run.phase != kOver) ++run.scroll;
    const Entity e = find_bird(w);
    if (!e) return;
    Bird &bird = w.get<Bird>(e);
    Vec2 &pos = w.get<Transform>(e).position;
    if (run.phase == kTitle) return;                            // main.cpp bobs the bird
    bird.vy = std::min(bird.vy + kGravity, kMaxFall);
    pos.y += bird.vy;
    if (run.phase == kOver) {                                   // fall onto the ground and rest
        if (pos.y > kGroundY - kBirdR) pos.y = kGroundY - kBirdR, bird.vy = 0;
        return;
    }
    if (pos.y < kBirdR) pos.y = kBirdR, bird.vy = 0;           // ceiling
    for (auto [pe, t, p] : w.view<Transform, Pipe>()) {
        t.position.x -= kScroll;
        if (t.position.x < -kPipeW) ctx.commands().destroy(pe);
        if (!p.passed && t.position.x + kPipeW * 0.5f < kBirdX - kBirdR) {
            p.passed = true;
            ++run.score;
            burst(ctx, &Fx::sparkle, {kBirdX, pos.y}, 12);
        }
        if (hits_pipe(pos, t, p)) {
            die(ctx, run, bird, pos);
            return;
        }
    }
    if (pos.y + kBirdR >= kGroundY) {
        pos.y = kGroundY - kBirdR;
        die(ctx, run, bird, pos);
        return;
    }
    if (run.next_pipe > 0 && --run.next_pipe > 0) return;
    run.next_pipe = kPipeEvery;
    constexpr int kLo = static_cast<int>(kGap * 0.5f) + 34;
    constexpr int kHi = static_cast<int>(kGroundY - kGap * 0.5f) - 26;
    const float gap = static_cast<float>(ctx.rng("flappy.pipes"_id).between(kLo, kHi));
    const Vec2 at{kW + kPipeW, gap};
    Commands &cmd = ctx.commands();
    const Entity pe = cmd.spawn();
    cmd.add(pe, Transform{.position = at});
    cmd.add(pe, PrevTransform{.position = at});
    cmd.add(pe, Pipe{.gap_y = gap});
}
}

Entity find_bird(World &w) {
    for (auto [e, b] : w.view<Bird>()) return e;
    return {};
}

void start_game(World &w) {
    w.resources().insert<Run>();
    w.snapshot_resource<Run>();
    const Entity bird = w.create();
    w.add<Transform>(bird, Transform{.position = {kBirdX, kStartY}});
    w.add<PrevTransform>(bird, PrevTransform{.position = {kBirdX, kStartY}});
    w.add<Bird>(bird);
}

void RulesPlugin::build(App &app) {
    app.require_plugin("lam.input", name());
    app.register_component<Bird>().register_component<Pipe>();
    app.add_event<BurstAt>();
    app.add_event<ShakeAt>();
    ActionMap &map = app.res<ActionMap>();
    const auto k = [](Key key) { return ActionMap::key(key); };
    app.insert_resource<Controls>(Controls{
        .flap = map.button("flap", {k(Key::Space), k(Key::Up), k(Key::W), ActionMap::mouse(MouseButton::Left),
                                    ActionMap::pad(PadButton::South)})});
    app.add_system({.stage = Stage::Input, .fn = controls, .name = "flappy.controls", .run_if = &has_local<Run>});
    app.add_system({.stage = Stage::Simulate, .fn = step, .name = "flappy.step", .run_if = &has_local<Run>});
}

// Aims a little below the centre of the next gap and flaps when it falls past that line.
// Gives up at 20, 30 or 40 points so a long run also shows game over and restart.
void Bot::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    InputFrame &f = frames[0];
    f.clear_devices();
    const Run *run = ctx.try_local<Run>();
    if (!ctx.focused() || run == nullptr) return;
    World &w = ctx.world();
    const Entity e = find_bird(w);
    if (!e) return;
    if (run->phase != kPlay) {                                  // start / restart
        if (run->phase_ticks % 30 == 29) f.press(controls.flap);
        return;
    }
    const float y = w.get<Transform>(e).position.y, vy = w.get<Bird>(e).vy;
    float aim = kStartY, nearest = 1e9f;
    for (auto [pe, t, p] : w.view<Transform, Pipe>()) {
        const float right = t.position.x + kPipeW * 0.5f;
        if (right < kBirdX - kBirdR || right > nearest) continue;
        nearest = right;
        aim = p.gap_y + kGap * 0.5f - 22.0f;
    }
    if (run->score >= 20 + 10 * (run->games % 3)) return;       // give up
    if (y + vy * 2.0f > aim && vy > -1.0f) f.press(controls.flap);
}
}
