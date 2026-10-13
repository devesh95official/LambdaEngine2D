#pragma once
// Tweens. Tween runs on tick time and is hashed, for gameplay motion. FrameTween runs on frame
// time and only touches presentation (RenderOffset, Sprite), for juice.
#include <array>
#include <cstdint>
#include <span>
#include <lam/core/id.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/color.h>
#include <lam/math/ease.h>
#include <lam/math/vec2.h>
namespace lam {
class Commands;
// Position/Scale/Rotation write the Transform (Tween) or RenderOffset (FrameTween); Alpha and
// Tint write Sprite::tint; Custom writes `custom`. Missing targets are skipped.
enum class TweenTarget : std::uint8_t { Position, Scale, Rotation, Alpha, Tint, Custom };
// One step: wait `delay` ticks, then play `repeat + 1` passes (255 = forever) of `ticks` ticks.
// `relative` offsets from/to by the target's value at start (not for Tint, or Alpha in a Tween).
struct TweenSpec {
    TweenTarget target = TweenTarget::Position;                 // what the step writes
    Vec2 from, to;                                              // Position/Scale; x only for Rotation/Alpha/Custom
    Color color_from, color_to;                                 // Tint only
    std::uint16_t ticks = 30;                                   // pass length (0: jumps to `to`)
    std::uint16_t delay = 0;                                    // ticks before the first pass
    Ease ease = Ease::OutQuad;                                  // applied to each pass's progress
    std::uint8_t repeat = 0;                                    // 255 = forever
    bool yoyo = false;
    bool relative = false;                                      // from/to added to the start value
    StrId tag;                                                  // reported in TweenFinished
};
// Up to 4 steps played on tick time. Tagged steps and the last step emit TweenFinished; the
// component removes itself when done.
struct Tween {                                                  // component (tick time)
    std::array<TweenSpec, 4> steps{};                           // steps[0..count) play in order
    std::uint8_t count = 0, current = 0;                        // current = the step playing
    std::uint16_t elapsed = 0;                                  // ticks since the step started (delay included)
    std::uint8_t loops_done = 0;                                // completed passes of the current step
    float custom = 0;                                           // TweenTarget::Custom output (game reads it)
};
// Same as Tween but on frame time (ticks x tick_dt seconds), presentation only, no events.
// Adds a RenderOffset if needed and starts a frame later.
struct FrameTween {                                             // component (frame time)
    std::array<TweenSpec, 4> steps{};                           // steps[0..count) play in order
    std::uint8_t count = 0, current = 0;                        // current = the step playing
    float elapsed_seconds = 0;                                  // seconds since the step started (delay included)
    std::uint8_t loops_done = 0;                                // completed passes of the current step
    float custom = 0;                                           // TweenTarget::Custom output (game reads it)
};
// Adds or replaces a one-step Tween on e (deferred).
void tween_to(Commands &c, Entity e, const TweenSpec &s);
// Up to 4 steps; an empty list removes the Tween.
void tween_sequence(Commands &c, Entity e, std::span<const TweenSpec> steps);
// Adds or replaces a one-step FrameTween, plus a RenderOffset if e has none.
void frame_tween_to(Commands &c, Entity e, const TweenSpec &s);
// FrameTween version of tween_sequence.
void frame_tween_sequence(Commands &c, Entity e, std::span<const TweenSpec> steps);
// Emitted in Animate -1300 for tagged steps and the last step of a Tween. FrameTween emits none.
struct TweenFinished { Entity entity; StrId tag; };             // World event
}
LAM_REFLECT_ENUM(lam::TweenTarget, "position", "scale", "rotation", "alpha", "tint", "custom")
LAM_REFLECT_STRUCT(lam::TweenSpec, target, from, to, color_from, color_to, ticks, delay, ease, repeat, yoyo,
                   relative, tag)
LAM_REFLECT(lam::Tween, steps, count, current, elapsed, loops_done, custom)
LAM_REFLECT_FLAGS(lam::FrameTween, lam::kSerialize | lam::kSnapshot, steps, count, current, elapsed_seconds,
                  loops_done, custom)
