#pragma once
// Frame-time float tweens and constant-rate faders for UI and scene transitions. Tick-time
// entity tweens are in lam/animation/tween.h.
#include <lam/math/ease.h>
namespace lam {
class FloatTween {
public:
    // Starts at `from`, reaches `to` after `seconds` (<= 0: immediately).
    void start(float from, float to, float seconds, Ease e = Ease::OutQuad);
    // Non-finite or negative dt is ignored.
    void update(float dt);
    float value() const;
    bool done() const;
private:
    float from_ = 0, to_ = 0, seconds_ = 0, elapsed_ = 0;
    Ease ease_ = Ease::Linear;
};
// Moves toward a target at a constant rate and settles exactly on it.
class Fader {
public:
    void set_target(float target, float units_per_second);
    void snap(float value);
    void update(float dt);
    float value() const;
    bool settled() const;
private:
    float value_ = 0, target_ = 0, rate_ = 1;
};
}
