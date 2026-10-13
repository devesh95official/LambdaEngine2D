// Breakout bot: predicts where the ball crosses the paddle line (folding the side-wall bounces)
// and steers there, hitting with a varying paddle offset so the ball spreads across the wall.
#include <algorithm>
#include <cmath>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/input/input_frame.h>
#include <lam/physics/components.h>
#include <breakout/breakout.h>

namespace breakout {
using namespace lam;

namespace {
float fold(float x, float lo, float hi) {                       // reflect x into [lo, hi]
    const float span = hi - lo;
    if (span <= 0) return lo;
    float m = std::fmod(x - lo, 2.0f * span);
    if (m < 0) m += 2.0f * span;
    return lo + (m > span ? 2.0f * span - m : m);
}
}

BotOut bot_decide(World &w) {                                  // reads only
    BotOut out;
    const Run *run = w.resources().try_get<Run>();
    if (run == nullptr) return out;
    if (run->over) {                                            // restart like a player would
        out.launch = (w.tick() % 2u) == 0u;
        return out;
    }
    Vec2 paddle{kFieldW * 0.5f, kPaddleY}, ball = paddle, vel;
    for (auto [e, tf] : w.view<Transform>(With<Paddle>{})) paddle = tf.position;
    for (auto [e, tf, v] : w.view<Transform, Velocity>(With<Ball>{})) {
        ball = tf.position;
        vel = v.linear;
    }
    float target = ball.x;
    if (!run->serving && vel.y > 0) {
        const float t = (kPaddleY - kPaddleHalf.y - kBallRadius - ball.y) / vel.y;
        if (t > 0) target = fold(ball.x + vel.x * t, kWall + kBallRadius, kFieldW - kWall - kBallRadius);
    }
    // Hit off-centre in a fixed cycle: the paddle centre sits beside the landing point.
    static constexpr float kOffsets[5] = {-12, 5, -5, 12, 0};
    if (!run->serving) target -= kOffsets[static_cast<std::size_t>(run->paddle_hits) % 5u];
    out.move_x = std::clamp((target - paddle.x) / 10.0f, -1.0f, 1.0f);
    if (std::fabs(out.move_x) < 0.1f) out.move_x = 0;
    out.launch = run->serving && (w.tick() % 2u) == 0u;          // a press, then a release tick
    return out;
}

void Bot::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    InputFrame &f = frames[0];
    f.clear_devices();                                          // drop the human's input, keep intents
    if (!ctx.focused() || ctx.try_local<Run>() == nullptr) return;
    const BotOut o = bot_decide(ctx.world());
    f.set_axis(controls.move, {o.move_x, 0});
    if (o.launch) f.press(controls.launch);
}
}
