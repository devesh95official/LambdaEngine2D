// Breakout rules (breakout_sim: pure targets only).
//   Input        breakout.steer    paddle intent, launch, restart
//   PostPhysics  breakout.bounce   ball reflections from CollisionStarted; bricks break
//   PostPhysics  breakout.serve    the served ball rides on the paddle
//   PostPhysics  breakout.outcome  ball lost, game over, level cleared
// The ball is a kinematic body (Velocity, no RigidBody): physics integrates it with no response
// and reports contacts; the rules reflect it. The paddle is a TopDownMotor blocked by the walls.
#include <algorithm>
#include <cmath>
#include <lam/core/app.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/component.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/input_frame.h>
#include <lam/input/keys.h>
#include <lam/physics/components.h>
#include <lam/physics/events.h>
#include <lam/physics/motors.h>
#include <breakout/breakout.h>

namespace breakout {
using namespace lam;
using namespace lam::literals;

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kMaxBounceRad = 60.0f * kPi / 180.0f;          // paddle edge -> 60 degrees off vertical

Vec2 brick_center(int col, int row) {
    return {kWall + 2.0f + kBrickPitchX * 0.5f + kBrickPitchX * static_cast<float>(col),
            kBrickTop + kBrickPitchY * static_cast<float>(row)};
}
Brick brick_for_row(int row) { return Brick{.row = row, .points = (kRows - row) * 10}; }
Collider brick_collider() {
    return Collider{.half = kBrickHalf, .layer = kLayerBrick, .mask = kLayerBall};
}
Vec2 serve_point(Vec2 paddle) { return {paddle.x, kPaddleY - kPaddleHalf.y - kBallRadius - 1.0f}; }

// Keeps the ball from flattening into a near-horizontal loop between the side walls.
Vec2 keep_steep(Vec2 v, float speed) {
    const float min_vy = 0.3f * speed;
    if (std::fabs(v.y) < min_vy) v.y = v.y < 0 ? -min_vy : min_vy;
    const float len = std::sqrt(v.x * v.x + v.y * v.y);
    return len > 0 ? Vec2{v.x / len * speed, v.y / len * speed} : Vec2{0, -speed};
}

void spawn_bricks(Commands &cmd) {
    for (int row = 0; row < kRows; ++row)
        for (int col = 0; col < kCols; ++col) {
            const Entity e = cmd.spawn();
            cmd.add(e, Transform{.position = brick_center(col, row)});
            cmd.add(e, brick_collider());
            cmd.add(e, brick_for_row(row));
        }
}

// Stage::Input: paddle intent every tick (motors keep the last intent), launch and restart.
void steer(Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    const float move_x = run.over ? 0.0f : std::clamp(in.axis2(c.move).x, -1.0f, 1.0f);
    for (auto [e, motor] : w.view<TopDownMotor>(With<Paddle>{})) motor.move = {move_x, 0};
    if (!in.is_pressed(c.launch)) return;
    if (run.over) {                                             // restart: same world, fresh wall
        Commands &cmd = ctx.commands();
        for (auto [e, brick] : w.view<Brick>()) cmd.destroy(e);
        spawn_bricks(cmd);
        for (auto [e, tf] : w.view<Transform>(With<Paddle>{})) cmd.teleport(e, {kFieldW * 0.5f, kPaddleY});
        Stats &stats = ctx.local<Stats>();
        ++stats.games;
        run = Run{.bricks_left = kCols * kRows};
        return;
    }
    if (!run.serving) return;
    const float deg = static_cast<float>(ctx.rng("serve"_id).below(61)) - 30.0f;   // -30..30
    const float a = deg * kPi / 180.0f;
    const float speed = ball_speed(run.level);
    for (auto [e, vel] : w.view<Velocity>(With<Ball>{})) vel.linear = {std::sin(a) * speed, -std::cos(a) * speed};
    run.serving = false;
}

// Stage::PostPhysics: contact events of this tick's Physics.
void bounce(Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();
    if (run.serving || run.over) return;
    for (const CollisionStarted &c : ctx.read<CollisionStarted>()) {
        const Entity ball = w.has<Ball>(c.a) ? c.a : (w.has<Ball>(c.b) ? c.b : Entity{});
        if (!ball) continue;
        const Entity hit = other(c, ball);
        if (!w.alive(hit)) continue;
        Vec2 &v = w.get<Velocity>(ball).linear;
        const float speed = ball_speed(run.level);
        if (w.has<Paddle>(hit)) {
            if (v.y <= 0) continue;                             // already going up
            const float dx = w.get<Transform>(ball).position.x - w.get<Transform>(hit).position.x;
            const float rel = std::clamp(dx / (kPaddleHalf.x + kBallRadius), -1.0f, 1.0f);
            const float a = rel * kMaxBounceRad;
            v = {std::sin(a) * speed, -std::cos(a) * speed};
            ++run.paddle_hits;
            continue;
        }
        const Vec2 n = normal_from(c, ball);                    // from the ball toward the wall/brick
        const float d = v.x * n.x + v.y * n.y;
        if (d > 0) v = {v.x - 2.0f * d * n.x, v.y - 2.0f * d * n.y};
        v = keep_steep(v, speed);
        if (const Brick *b = w.try_get<Brick>(hit)) {
            run.score += b->points;
            --run.bricks_left;
            ++run.bricks_broken;
            Stats &stats = ctx.local<Stats>();
            ++stats.total_bricks;
            stats.best_score = std::max(stats.best_score, run.score);
            ctx.commands().destroy(hit);
        }
    }
    // Containment: one tick can report several contacts (a paddle edge beside a side wall); the
    // paddle angle may then point back into the wall the ball already touches, and a touch that
    // continues reports no new CollisionStarted. Never let the ball move into a wall it overlaps.
    for (auto [e, tf, vel] : w.view<Transform, Velocity>(With<Ball>{})) {
        const Vec2 p = tf.position;
        Vec2 &v = vel.linear;
        if (p.x - kBallRadius < kWall && v.x < 0) v.x = -v.x;
        if (p.x + kBallRadius > kFieldW - kWall && v.x > 0) v.x = -v.x;
        if (p.y - kBallRadius < kWall && v.y < 0) v.y = -v.y;
    }
}

// Stage::PostPhysics (after bounce): while serving, the ball sits on the paddle.
void serve(Ctx &ctx) {
    World &w = ctx.world();
    if (!ctx.local<Run>().serving) return;
    Vec2 paddle{kFieldW * 0.5f, kPaddleY};
    for (auto [e, tf] : w.view<Transform>(With<Paddle>{})) paddle = tf.position;
    for (auto [e, tf, vel] : w.view<Transform, Velocity>(With<Ball>{})) {
        tf.position = serve_point(paddle);
        vel.linear = {};
    }
}

// Stage::PostPhysics (last): lost ball, game over, cleared wall.
void outcome(Ctx &ctx) {
    World &w = ctx.world();
    Run &run = ctx.local<Run>();
    if (run.over) return;
    if (!run.serving) {
        for (auto [e, tf, vel] : w.view<Transform, Velocity>(With<Ball>{})) {
            if (tf.position.y - kBallRadius <= kFieldH) continue;
            ++run.balls_lost;
            --run.lives;
            vel.linear = {};
            if (run.lives <= 0) {
                run.lives = 0;
                run.over = true;
            } else {
                run.serving = true;
            }
        }
    }
    if (run.bricks_left <= 0 && !run.over) {                    // next level: a fresh wall, faster ball
        ++run.level;
        run.bricks_left = kCols * kRows;
        run.serving = true;
        spawn_bricks(ctx.commands());
        for (auto [e, vel] : w.view<Velocity>(With<Ball>{})) vel.linear = {};
    }
}

void add_rule(App &app, Stage s, SystemFn fn, std::string_view name, int order) {
    app.add_system({.stage = s, .fn = fn, .name = name, .order = order, .run_if = &has_local<Run>});
}
}

float ball_speed(std::int32_t level) { return std::min(150.0f + 15.0f * static_cast<float>(level - 1), 240.0f); }

void RulesPlugin::build(App &app) {
    app.require_plugin("lam.physics", name());
    app.require_plugin("lam.input", name());
    app.register_component<Paddle>().register_component<Wall>().register_component<Ball>().register_component<Brick>();
    ActionMap &map = app.res<ActionMap>();
    const auto k = [](Key key) { return ActionMap::key(key); };
    app.insert_resource<Controls>(Controls{
        .move = map.axis2("move", {{k(Key::W), k(Key::S), k(Key::A), k(Key::D)},
                                   {k(Key::Up), k(Key::Down), k(Key::Left), k(Key::Right)}},
                          {{PadAxis::LeftX, PadAxis::LeftY}}),
        .launch = map.button("launch", {k(Key::Space), ActionMap::pad(PadButton::South)})});
    add_rule(app, Stage::Input, steer, "breakout.steer", 0);
    add_rule(app, Stage::PostPhysics, bounce, "breakout.bounce", 0);
    add_rule(app, Stage::PostPhysics, serve, "breakout.serve", 10);
    add_rule(app, Stage::PostPhysics, outcome, "breakout.outcome", 20);
}

void start_game(World &w) {
    const auto wall = [&](Vec2 centre, Vec2 half) {
        const Entity e = w.create();
        w.add<Transform>(e, Transform{.position = centre});
        w.add<Collider>(e, Collider{.half = half, .layer = kLayerWall, .mask = kLayerBall});
        w.add<Wall>(e);
    };
    wall({kWall * 0.5f, kFieldH * 0.5f}, {kWall * 0.5f, kFieldH * 0.5f + 16});            // left
    wall({kFieldW - kWall * 0.5f, kFieldH * 0.5f}, {kWall * 0.5f, kFieldH * 0.5f + 16});  // right
    wall({kFieldW * 0.5f, kWall * 0.5f}, {kFieldW * 0.5f, kWall * 0.5f});                 // top

    const Entity paddle = w.create();
    w.add<Transform>(paddle, Transform{.position = {kFieldW * 0.5f, kPaddleY}});
    w.add<Velocity>(paddle);
    w.add<Collider>(paddle, Collider{.half = kPaddleHalf, .layer = kLayerPaddle, .mask = kLayerBall});
    w.add<TopDownMotor>(paddle, TopDownMotor{.max_speed = 210, .accel = 2400, .decel = 3000, .corner_assist = 0,
                                             .solid_mask = kLayerWall});
    w.add<Paddle>(paddle);

    const Entity ball = w.create();
    w.add<Transform>(ball, Transform{.position = serve_point({kFieldW * 0.5f, kPaddleY})});
    w.add<Velocity>(ball);
    w.add<Collider>(ball, Collider{.shape = ShapeType::Circle, .radius = kBallRadius, .layer = kLayerBall,
                                   .mask = kLayerWall | kLayerPaddle | kLayerBrick});
    w.add<Ball>(ball);

    for (int row = 0; row < kRows; ++row)
        for (int col = 0; col < kCols; ++col) {
            const Entity e = w.create();
            w.add<Transform>(e, Transform{.position = brick_center(col, row)});
            w.add<Collider>(e, brick_collider());
            w.add<Brick>(e, brick_for_row(row));
        }

    w.resources().insert<Run>(Run{.bricks_left = kCols * kRows});
    w.snapshot_resource<Run>();
    w.resources().insert<Stats>();
    w.snapshot_resource<Stats>();
}
}
