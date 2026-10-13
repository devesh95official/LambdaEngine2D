#include <lam/math/curve.h>

#include <cmath>

namespace lam {

void ColorCurve::add(float t, Color c) {
    if (count_ >= keys_.size() || !std::isfinite(t)) return;
    t = t - std::floor(t);
    std::size_t i = count_;
    while (i > 0 && keys_[i - 1].t > t) {
        keys_[i] = keys_[i - 1];
        --i;
    }
    keys_[i] = {t, c};
    ++count_;
}

Color ColorCurve::sample(float t) const {
    if (count_ == 0) return kWhite;
    if (count_ == 1 || !std::isfinite(t)) return keys_[0].color;
    t = t - std::floor(t);
    // Find the last key at or before t (wrapping to the last key when t precedes the first).
    std::size_t lo = count_ - 1;
    for (std::size_t i = 0; i < count_; ++i)
        if (keys_[i].t <= t) lo = i;
    const std::size_t hi = (lo + 1) % count_;
    float span = keys_[hi].t - keys_[lo].t;
    float into = t - keys_[lo].t;
    if (span <= 0.0f) span += 1.0f;
    if (into < 0.0f) into += 1.0f;
    return lerp(keys_[lo].color, keys_[hi].color, span > 0.0f ? into / span : 0.0f);
}

int ColorCurve::size() const { return count_; }

} // namespace lam
