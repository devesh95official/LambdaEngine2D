// Pong rules and the --bot driver. Effects go out as BurstAt / ShakeAt events.
#include <pong/pong.h>

#include <algorithm>
#include <cmath>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/keys.h>
#include <lam/render/camera_math.h>

namespace pong {
using namespace lam;
using namespace lam::literals;

namespace {
constexpr float kPlayerSpeed = 4.6f;                            // px per tick
constexpr float kCpuSpeed = 2.7f;
constexpr float kServeSpeed = 4.0f, kSpeedUp = 1.07f, kMaxSpeed = 9.5f;
constexpr float kMaxBounce = 0.95f;                             // radians at the paddle's tip
constexpr std::uint32_t kServeTicks = 50;

float clamp_pad(float y) { return std::clamp(y, kTop, kBottom - kPadH); }

void set_phase(Match &m, std::int32_t phase) {
    m.phase = phase;
    m.phase_ticks = 0;
}

void reset_ball(Match &m) {
    m.ball = {(kW - kBall) / 2, (kTop + kBottom - kBall) / 2};
    m.prev_ball = m.ball;
    m.vel = {};
    m.rally = 0;
}

void new_match(Match &m) {
    m.score1 = m.score2 = 0;
    m.winner = 0;
    m.p1 = m.p2 = m.prev_p1 = m.prev_p2 = (kTop + kBottom - kPadH) / 2;
    reset_ball(m);
    set_phase(m, kServe);
}

void emit_burst(const Ctx &ctx, AssetId<EmitterConfig> id, Vec2 at, std::uint16_t n) {
    if (const Fx *fx = ctx.try_res<Fx>(); fx != nullptr && id) ctx.emit(BurstAt{id, at, n});
}

// Sweep the ball across the paddle face so a fast ball can't tunnel through.
bool paddle_hit(const Ctx &ctx, Match &m, float pad_top, bool left) {
    const float face = left ? kLeftFront : kRightFront;
    const float before = left ? m.prev_ball.x : m.prev_ball.x + kBall;
    const float after = left ? m.ball.x : m.ball.x + kBall;
    const bool crossed = left ? (before >= face && after < face) : (before <= face && after > face);
    if (!crossed) return false;
    const float t = (face - before) / (after - before);
    const float y = m.prev_ball.y + (m.ball.y - m.prev_ball.y) * t;
    if (y + kBall < pad_top || y > pad_top + kPadH) return false;
    const float off = std::clamp(((y + kBall / 2) - (pad_top + kPadH / 2)) / (kPadH / 2 + kBall / 2), -1.0f, 1.0f);
    m.speed = std::min(m.speed * kSpeedUp, kMaxSpeed);
    const float a = off * kMaxBounce;
    m.vel = {(left ? 1.0f : -1.0f) * m.speed * std::cos(a), m.speed * std::sin(a)};
    m.ball = {left ? face : face - kBall, y};
    ++m.rally;
    ++m.hits;
    m.best_rally = std::max(m.best_rally, m.rally);
    m.ai_error = ctx.rng("pong.ai"_id).range(-22.0f, 22.0f);
    if (const Fx *fx = ctx.try_res<Fx>())
        emit_burst(ctx, left ? fx->hit1 : fx->hit2, {face, y + kBall / 2}, 14);
    if (m.speed > 7.0f) ctx.emit(ShakeAt{.trauma = 0.15f});
    return true;
}

void score(const Ctx &ctx, Match &m, int who) {
    (who == 1 ? m.score1 : m.score2) += 1;
    (who == 1 ? m.points1 : m.points2) += 1;
    if (const Fx *fx = ctx.try_res<Fx>())
        emit_burst(ctx, fx->goal, {who == 1 ? kW - 4 : 4.0f, m.ball.y + kBall / 2}, 40);
    ctx.emit(ShakeAt{.trauma = 0.55f});
    m.serve_dir = who == 1 ? 1 : -1;                            // serve toward the player who conceded
    reset_ball(m);
    if (m.score1 >= kWinScore || m.score2 >= kWinScore) {
        m.winner = who;
        ++m.matches;
        if (who == 1 && !m.two_player) ++m.wins;
        set_phase(m, kOver);
    } else {
        set_phase(m, kServe);
    }
}

void step(Ctx &ctx) {
    Match &m = ctx.local<Match>();
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    m.prev_p1 = m.p1;
    m.prev_p2 = m.p2;
    m.prev_ball = m.ball;
    ++m.phase_ticks;

    if (m.phase == kTitle) {
        if (in.is_pressed(c.mode) || in.is_pressed(c.p1_up) || in.is_pressed(c.p1_down) ||
            in.is_pressed(c.p2_up) || in.is_pressed(c.p2_down))
            m.two_player = !m.two_player;
        if (in.is_pressed(c.start)) new_match(m);
        return;
    }
    if (m.phase == kOver) {
        if (m.phase_ticks < 30) return;                         // ignore the key that was held
        if (in.is_pressed(c.start)) new_match(m);
        else if (in.is_pressed(c.title) || in.is_pressed(c.mode)) set_phase(m, kTitle);
        return;
    }

    // 1P: W/S, Up/Down or the mouse move the left paddle. 2P: Up/Down is player 2.
    const bool up1 = in.is_held(c.p1_up) || (!m.two_player && in.is_held(c.p2_up));
    const bool dn1 = in.is_held(c.p1_down) || (!m.two_player && in.is_held(c.p2_down));
    if (up1 || dn1) m.mouse_mode = false;
    if (in.pointer_valid && !m.two_player) {
        if (m.mouse_y >= 0 && std::abs(in.pointer_world.y - m.mouse_y) > 0.5f) m.mouse_mode = true;
        m.mouse_y = in.pointer_world.y;
    }
    if (m.mouse_mode) {
        const float d = (m.mouse_y - kPadH / 2) - m.p1;
        m.p1 += std::clamp(d, -kPlayerSpeed * 1.5f, kPlayerSpeed * 1.5f);
    } else {
        m.p1 += (static_cast<float>(dn1) - static_cast<float>(up1)) * kPlayerSpeed;
    }
    if (m.two_player) {
        m.p2 += (static_cast<float>(in.is_held(c.p2_down)) - static_cast<float>(in.is_held(c.p2_up))) * kPlayerSpeed;
    } else {                                                    // CPU chases the ball once it heads over
        const float centre = m.p2 + kPadH / 2;
        const float target = m.vel.x > 0 && m.ball.x > kW * 0.4f ? m.ball.y + kBall / 2 + m.ai_error : (kTop + kBottom) / 2;
        m.p2 += std::clamp(target - centre, -kCpuSpeed, kCpuSpeed);
    }
    m.p1 = clamp_pad(m.p1);
    m.p2 = clamp_pad(m.p2);

    if (m.phase == kServe) {
        if (m.phase_ticks < kServeTicks) return;
        const float a = ctx.rng("pong.serve"_id).range(-0.5f, 0.5f);
        m.speed = kServeSpeed;
        m.vel = {static_cast<float>(m.serve_dir) * m.speed * std::cos(a), m.speed * std::sin(a)};
        set_phase(m, kPlay);
        return;
    }

    // Ball: walls, then paddles, then goals.
    m.ball += m.vel;
    if (m.ball.y < kTop) {
        m.ball.y = 2 * kTop - m.ball.y;
        m.vel.y = std::abs(m.vel.y);
    } else if (m.ball.y + kBall > kBottom) {
        m.ball.y = 2 * (kBottom - kBall) - m.ball.y;
        m.vel.y = -std::abs(m.vel.y);
    }
    if (m.vel.x < 0) paddle_hit(ctx, m, m.p1, true);
    else paddle_hit(ctx, m, m.p2, false);
    if (m.ball.x + kBall < 0) score(ctx, m, 2);
    else if (m.ball.x > kW) score(ctx, m, 1);
}
}

void start(World &w) {
    w.resources().insert<Match>();
    w.snapshot_resource<Match>();
}

void RulesPlugin::build(App &app) {
    app.require_plugin("lam.input", name());
    // Registered here too so the rules run without the particle and render plugins.
    app.add_event<BurstAt>();
    app.add_event<ShakeAt>();
    ActionMap &map = app.res<ActionMap>();
    const auto key = [](Key k) { return ActionMap::key(k); };
    app.insert_resource<Controls>(Controls{
        .p1_up = map.button("p1_up", {key(Key::W)}),
        .p1_down = map.button("p1_down", {key(Key::S)}),
        .p2_up = map.button("p2_up", {key(Key::Up)}),
        .p2_down = map.button("p2_down", {key(Key::Down)}),
        .start = map.button("start", {key(Key::Space), key(Key::Enter), ActionMap::mouse(MouseButton::Left)}),
        .mode = map.button("mode", {key(Key::Tab), key(Key::M)}),
        .title = map.button("title", {key(Key::T), key(Key::Backspace)})});
    app.add_system({.stage = Stage::Simulate, .fn = step, .name = "pong.step", .run_if = &has_local<Match>});
}

// ---- Bot -------------------------------------------------------------------------------------
namespace {
// Where the ball's centre will cross the left face, folding wall bounces.
float predict_left(const Match &m) {
    if (m.vel.x >= 0) return (kTop + kBottom) / 2;
    const float t = (m.ball.x - kLeftFront) / -m.vel.x;
    float y = m.ball.y + m.vel.y * t - kTop;
    const float span = kBottom - kTop - kBall;
    const float period = 2 * span;
    y = std::fmod(y, period);
    if (y < 0) y += period;
    if (y > span) y = period - y;
    return y + kTop + kBall / 2;
}
}

void Bot::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    const Match *m = ctx.world().resources().try_get<Match>();
    const Controls *c = ctx.try_res<Controls>();
    if (m == nullptr || c == nullptr) return;
    InputFrame &f = frames[0];
    f.clear_devices();                                          // ignore keyboard and mouse
    if (m->phase == kTitle) {
        if (m->two_player) f.press(c->mode);                    // the bot plays the CPU
        else if (m->phase_ticks > 40) f.press(c->start);
        return;
    }
    if (m->phase == kOver) {
        if (m->phase_ticks == 120) f.press(c->start);
        return;
    }
    // Hit a bit off-centre, by an amount that changes each rally, so returns vary.
    const float aim = predict_left(*m) + static_cast<float>(static_cast<int>(m->rally % 5) - 2) * 8.0f;
    const float d = aim - (m->p1 + kPadH / 2);
    if (d < -3) f.hold(c->p1_up);
    else if (d > 3) f.hold(c->p1_down);
}
}
