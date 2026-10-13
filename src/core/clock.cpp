#include <lam/core/clock.h>

#include <algorithm>
#include <cmath>

namespace lam {

FixedClock::FixedClock(int ticks_per_second, double max_frame_seconds, int max_ticks_per_frame)
    : ticks_per_second_(std::max(1, ticks_per_second)),
      max_ticks_per_frame_(std::max(1, max_ticks_per_frame)),
      tick_seconds_(1.0 / static_cast<double>(std::max(1, ticks_per_second))),
      max_frame_seconds_(std::max(0.0, max_frame_seconds)) {}

int FixedClock::advance(double frame_seconds) {
    if (!(frame_seconds > 0.0) || !std::isfinite(frame_seconds)) return 0;
    banked_ += std::min(frame_seconds, max_frame_seconds_);
    int ticks = 0;
    while (banked_ >= tick_seconds_ && ticks < max_ticks_per_frame_) {
        banked_ -= tick_seconds_;
        ++ticks;
    }
    // A hitch: drop the excess so the next frames do not keep catching up.
    if (banked_ >= tick_seconds_) banked_ = std::fmod(banked_, tick_seconds_);
    return ticks;
}

float FixedClock::alpha() const {
    return static_cast<float>(std::clamp(banked_ / tick_seconds_, 0.0, 0.99999));
}

void FixedClock::reset() { banked_ = 0.0; }

int FixedClock::ticks_per_second() const { return ticks_per_second_; }

float FixedClock::tick_seconds() const { return static_cast<float>(tick_seconds_); }

} // namespace lam
