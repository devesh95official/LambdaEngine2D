#pragma once
// Keyframed colour over a wrapping [0, 1) domain, e.g. time-of-day ambient light.
#include <array>
#include <cstdint>
#include <lam/math/color.h>
namespace lam {
struct ColorKey { float t = 0; Color color; };
class ColorCurve {
public:
    // t wrapped into [0, 1); keys kept sorted; at most 16 (later ones ignored).
    void add(float t, Color c);
    // Lerps between the keys around t, wrapping from the last key to the first. Empty -> kWhite.
    Color sample(float t) const;
    int size() const;
private:
    std::array<ColorKey, 16> keys_{};
    std::uint8_t count_ = 0;
};
}
