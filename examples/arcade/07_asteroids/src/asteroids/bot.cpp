// --bot: presses the same actions a player would, decided from world state only.
#include <asteroids/game.h>

#include <cmath>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/math/scalar.h>

namespace ast {
using namespace lam;

void Bot::fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) {
    World &w = ctx.world();
    const Controls *c = ctx.try_res<Controls>();
    const Run *run = w.resources().try_get<Run>();
    if (c == nullptr || run == nullptr) return;
    InputFrame &f = frames[0];
    f.clear_devices();                                          // ignore keyboard and mouse
    if (run->phase != kPlay) {
        if (w.tick() % 60 == 30) f.press(c->start);
        return;
    }
    const Entity me = find_ship(w);
    if (!me) return;
    const Transform &t = w.get<Transform>(me);
    const Body &b = w.get<Body>(me);
    float best = 1e12f, threat = 1e12f;
    Vec2 aim{}, threat_dir{};
    for (auto [e, rt, rb, rock] : w.view<Transform, Body, Rock>()) {
        const Vec2 d = wrap_delta(t.position, rt.position);
        const float dist = length(d) - rb.radius;
        const Vec2 lead = d + (rb.vel - b.vel) * (length(d) / 380.0f);   // lead the target
        const float score = dist - (rock.size == 1 ? 20.0f : 0.0f);      // small rocks are fast, go for them first
        if (score < best) {
            best = score;
            aim = lead;
        }
        if (dist < threat) {
            threat = dist;
            threat_dir = d;
        }
    }
    if (best >= 1e12f) return;                                  // nothing to shoot
    float diff = angle_of(aim) - t.rotation;
    while (diff > kPi) diff -= kTau;
    while (diff < -kPi) diff += kTau;
    if (diff > 0.06f) f.hold(c->right);
    else if (diff < -0.06f) f.hold(c->left);
    if (std::fabs(diff) < 0.3f) f.hold(c->fire);
    // Thrust away from a close rock if we're facing away from it, up to a speed cap.
    const bool facing_away = dot(from_angle(t.rotation), threat_dir) < 0;
    if (threat < 50.0f && facing_away && length(b.vel) < 110.0f) f.hold(c->thrust);
}
}
