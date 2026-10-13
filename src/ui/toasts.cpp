// Toast notifications.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <lam/ui/toasts.h>

namespace lam {
namespace {

constexpr float kFadeIn = 0.15f;
constexpr float kFadeOut = 0.4f;
constexpr float kMinSeconds = 0.6f;

float alpha_of(const ToastQueue::Toast &t) {
    float a = 1.0f;
    if (t.age < kFadeIn) a = std::max(0.0f, t.age / kFadeIn);
    if (t.remaining < kFadeOut) a = std::min(a, std::max(0.0f, t.remaining / kFadeOut));
    return a;
}

} // namespace

ToastQueue::ToastQueue(std::size_t capacity) : capacity_(std::max<std::size_t>(1, capacity)) {
    toasts_.reserve(capacity_);
}

void ToastQueue::push(std::string_view text, float seconds, Color c) {
    if (!std::isfinite(seconds)) seconds = 2.5f;
    seconds = std::max(kMinSeconds, seconds);
    const FixedString<64> key(text);
    for (Toast &t : toasts_) {
        if (t.text == key) {
            if (t.count < 0xFFFFu) ++t.count;
            t.age = std::min(t.age, kFadeIn);                   // stays fully visible
            t.remaining = std::max(t.remaining, seconds);
            t.color = c;
            t.alpha = alpha_of(t);
            return;
        }
    }
    if (toasts_.size() >= capacity_) toasts_.erase(toasts_.begin());   // drop the oldest (no allocation)
    Toast t;
    t.text = key;
    t.color = c;
    t.remaining = seconds;
    t.age = 0.0f;
    t.count = 1;
    t.alpha = alpha_of(t);
    toasts_.push_back(t);
}

void ToastQueue::update(float dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return;
    for (Toast &t : toasts_) {
        t.age += dt;
        t.remaining -= dt;
        t.alpha = alpha_of(t);
    }
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [](const Toast &t) { return t.remaining <= 0.0f; }),
                  toasts_.end());
}

std::span<const ToastQueue::Toast> ToastQueue::toasts() const {
    return toasts_;
}

void ToastQueue::clear() {
    toasts_.clear();
}

std::size_t ToastQueue::capacity() const {
    return capacity_;
}

} // namespace lam
