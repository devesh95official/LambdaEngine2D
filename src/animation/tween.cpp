// Tween (tick time, hashed) and FrameTween (frame time, presentation). No allocation: values are
// written in place and removal goes through the pre-reserved Commands arena.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include <lam/animation/tween.h>
#include <lam/core/assert.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/render/sprite.h>

#include "detail/animation_detail.h"

namespace lam {
namespace {

constexpr std::uint8_t kForever = 255;
constexpr std::size_t kMaxSteps = 4;

bool needs_offset(TweenTarget t) {
    return t == TweenTarget::Position || t == TweenTarget::Scale || t == TweenTarget::Rotation;
}

std::uint8_t to_alpha(float a) {
    return static_cast<std::uint8_t>(std::floor(clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}

// The eased progress of a pass at linear progress p (yoyo odd passes run backwards in time).
float eased(const TweenSpec &s, std::uint8_t loops_done, float p) {
    const float q = (s.yoyo && (loops_done & 1u) != 0) ? 1.0f - p : p;
    return ease(s.ease, q);
}

// Where a target's values live: the tick tween writes a Transform, the frame tween a RenderOffset.
struct Targets {
    Vec2 *position = nullptr;
    Vec2 *scale = nullptr;
    float *rotation = nullptr;
    Sprite *sprite = nullptr;
    float *custom = nullptr;
};

// Turns a relative step into absolute values from the target's current value. A tick Tween's
// Alpha ignores the Sprite: its value depends on frame pacing and must stay out of hashed state.
void capture_base(TweenSpec &s, const Targets &t, bool tick) {
    if (!s.relative) return;
    Vec2 base{};
    switch (s.target) {
    case TweenTarget::Position: base = t.position ? *t.position : Vec2{}; break;
    case TweenTarget::Scale: base = t.scale ? *t.scale : Vec2{}; break;
    case TweenTarget::Rotation: base = {t.rotation ? *t.rotation : 0.0f, 0.0f}; break;
    case TweenTarget::Alpha:
        if (!tick) base = {t.sprite ? static_cast<float>(t.sprite->tint.a) / 255.0f : 0.0f, 0.0f};
        break;
    case TweenTarget::Custom: base = {t.custom ? *t.custom : 0.0f, 0.0f}; break;
    case TweenTarget::Tint: break;
    }
    s.from += base;
    s.to += base;
    s.relative = false;
}

void apply(const TweenSpec &s, float e, const Targets &t) {
    switch (s.target) {
    case TweenTarget::Position:
        if (t.position) *t.position = lerp(s.from, s.to, e);
        break;
    case TweenTarget::Scale:
        if (t.scale) *t.scale = lerp(s.from, s.to, e);
        break;
    case TweenTarget::Rotation:
        if (t.rotation) *t.rotation = lerp(s.from.x, s.to.x, e);
        break;
    case TweenTarget::Alpha:
        if (t.sprite) t.sprite->tint.a = to_alpha(lerp(s.from.x, s.to.x, e));
        break;
    case TweenTarget::Tint:
        if (t.sprite) t.sprite->tint = lerp(s.color_from, s.color_to, e);
        break;
    case TweenTarget::Custom:
        if (t.custom) *t.custom = lerp(s.from.x, s.to.x, e);
        break;
    }
}

// True if another pass of the step follows the one that just completed (loops_done counts it).
bool more_passes(const TweenSpec &s, std::uint8_t loops_done) {
    return s.repeat == kForever || loops_done <= s.repeat;
}

template <class T> void record_steps(Commands &c, Entity e, std::span<const TweenSpec> steps) {
    LAM_ASSERT(steps.size() <= kMaxSteps, "tween_sequence: at most 4 steps");
    if (steps.empty()) {
        c.remove<T>(e);
        return;
    }
    T t;
    const std::size_t n = std::min(steps.size(), kMaxSteps);
    for (std::size_t i = 0; i < n; ++i) t.steps[i] = steps[i];
    t.count = static_cast<std::uint8_t>(n);
    c.add<T>(e, t);
}

} // namespace

void tween_to(Commands &c, Entity e, const TweenSpec &s) {
    record_steps<Tween>(c, e, std::span<const TweenSpec>(&s, 1));
}

void tween_sequence(Commands &c, Entity e, std::span<const TweenSpec> steps) {
    record_steps<Tween>(c, e, steps);
}

void frame_tween_to(Commands &c, Entity e, const TweenSpec &s) {
    frame_tween_sequence(c, e, std::span<const TweenSpec>(&s, 1));
}

void frame_tween_sequence(Commands &c, Entity e, std::span<const TweenSpec> steps) {
    bool offset = false;
    for (std::size_t i = 0; i < steps.size() && i < kMaxSteps; ++i) offset |= needs_offset(steps[i].target);
    if (offset && !c.world().has<RenderOffset>(e)) c.add<RenderOffset>(e, RenderOffset{});
    record_steps<FrameTween>(c, e, steps);
}

namespace detail {

bool tick_tween(Tween &t, Transform *xf, Sprite *sprite, StepDoneSink done, void *user) {
    if (t.count == 0 || t.count > kMaxSteps || t.current >= t.count) return true;
    const Targets targets{xf ? &xf->position : nullptr, xf ? &xf->scale : nullptr,
                          xf ? &xf->rotation : nullptr, sprite, &t.custom};
    TweenSpec &s = t.steps[t.current];
    const std::uint32_t ticks = s.ticks;
    const std::uint32_t delay = std::min<std::uint32_t>(s.delay, 65535u - ticks);
    if (t.elapsed < 65535u) ++t.elapsed;
    if (t.elapsed <= delay) return false;
    capture_base(s, targets, true);
    const std::uint32_t pass_tick = t.elapsed - delay; // 1..ticks
    const bool pass_done = pass_tick >= ticks;
    const float p = pass_done ? 1.0f : static_cast<float>(pass_tick) / static_cast<float>(ticks);
    apply(s, eased(s, t.loops_done, p), targets);
    if (!pass_done) return false;
    t.loops_done = static_cast<std::uint8_t>(t.loops_done + 1); // wraps only when forever (parity kept)
    if (more_passes(s, t.loops_done)) {
        t.elapsed = static_cast<std::uint16_t>(delay); // later passes skip the delay
        return false;
    }
    const bool last = t.current + 1 >= t.count;
    if (done != nullptr) done(user, s.tag, last);
    t.loops_done = 0;
    t.elapsed = 0;
    ++t.current;
    return last;
}

bool tick_frame_tween(FrameTween &t, float dt, float tick_dt, RenderOffset *offset, Sprite *sprite) {
    if (t.count == 0 || t.count > kMaxSteps || t.current >= t.count) return true;
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    if (!std::isfinite(tick_dt) || tick_dt < 0.0f) tick_dt = 0.0f;
    const Targets targets{offset ? &offset->offset : nullptr, offset ? &offset->scale : nullptr,
                          offset ? &offset->rotation : nullptr, sprite, &t.custom};
    // Float sums of frame_dt land a hair under whole tick counts: accept that much early.
    const float slack = tick_dt * 1e-3f;
    t.elapsed_seconds += dt;
    for (int guard = 0; guard < 16; ++guard) {
        TweenSpec &s = t.steps[t.current];
        const float delay = static_cast<float>(s.delay) * tick_dt;
        const float duration = static_cast<float>(s.ticks) * tick_dt;
        if (t.elapsed_seconds + slack < delay) return false;
        capture_base(s, targets, false);
        const float in_pass = std::max(t.elapsed_seconds - delay, 0.0f);
        if (in_pass + slack < duration) {
            apply(s, eased(s, t.loops_done, in_pass / duration), targets);
            return false;
        }
        apply(s, eased(s, t.loops_done, 1.0f), targets);
        const float carry = std::max(in_pass - duration, 0.0f);
        t.loops_done = static_cast<std::uint8_t>(t.loops_done + 1);
        if (more_passes(s, t.loops_done)) {
            t.elapsed_seconds = delay + carry;
            continue;
        }
        t.loops_done = 0;
        t.elapsed_seconds = carry;
        if (++t.current >= t.count) return true;
    }
    // Many zero-length passes in one frame: drop the remaining time.
    t.elapsed_seconds = 0.0f;
    return false;
}

namespace {

struct DoneCtx {
    Ctx *ctx;
    Entity entity;
};

void emit_finished(void *user, StrId tag, bool last) {
    const DoneCtx &d = *static_cast<const DoneCtx *>(user);
    if (tag || last) d.ctx->emit(TweenFinished{d.entity, tag});
}

} // namespace

void tweens_system(Ctx &ctx) {
    World &w = ctx.world();
    w.view<Tween>().each([&](Entity e, Tween &t) {
        Transform *xf = nullptr;
        if (Parent *p = w.try_get<Parent>(e)) xf = &p->local;
        else xf = w.try_get<Transform>(e);
        DoneCtx d{&ctx, e};
        if (tick_tween(t, xf, w.try_get<Sprite>(e), &emit_finished, &d)) ctx.commands().remove<Tween>(e);
    });
}

void frame_tweens_system(Ctx &ctx) {
    World &w = ctx.world();
    const float dt = ctx.time().frame_dt;
    const float tick_dt = ctx.time().tick_dt;
    w.view<FrameTween>().each([&](Entity e, FrameTween &t) {
        RenderOffset *offset = w.try_get<RenderOffset>(e);
        if (offset == nullptr && t.current < t.count && t.count <= kMaxSteps &&
            needs_offset(t.steps[t.current].target)) {
            // Saves and prefabs bring the FrameTween without its RenderOffset (snapshot only).
            // Add one and start next frame so a relative step has a base.
            ctx.commands().add<RenderOffset>(e, RenderOffset{});
            return;
        }
        if (tick_frame_tween(t, dt, tick_dt, offset, w.try_get<Sprite>(e)))
            ctx.commands().remove<FrameTween>(e);
    });
}

} // namespace detail
} // namespace lam
