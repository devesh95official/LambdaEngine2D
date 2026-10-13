#include <lam/math/tween.h>

#include <cmath>

#include <lam/math/scalar.h>

namespace lam {

void FloatTween::start(float from, float to, float seconds, Ease e) {
    from_ = from;
    to_ = to;
    seconds_ = seconds > 0.0f && std::isfinite(seconds) ? seconds : 0.0f;
    elapsed_ = 0.0f;
    ease_ = e;
}

void FloatTween::update(float dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return;
    elapsed_ = elapsed_ + dt < seconds_ ? elapsed_ + dt : seconds_;
}

float FloatTween::value() const {
    if (seconds_ <= 0.0f || elapsed_ >= seconds_) return to_;
    return lerp(from_, to_, ease(ease_, elapsed_ / seconds_));
}

bool FloatTween::done() const { return elapsed_ >= seconds_; }

void Fader::set_target(float target, float units_per_second) {
    if (std::isfinite(target)) target_ = target;
    rate_ = units_per_second > 0.0f && std::isfinite(units_per_second) ? units_per_second : 0.0f;
    if (rate_ == 0.0f) value_ = target_;
}

void Fader::snap(float value) {
    if (!std::isfinite(value)) return;
    value_ = value;
    target_ = value;
}

void Fader::update(float dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return;
    value_ = approach(value_, target_, rate_ * dt);
}

float Fader::value() const { return value_; }

bool Fader::settled() const { return value_ == target_; }

} // namespace lam
