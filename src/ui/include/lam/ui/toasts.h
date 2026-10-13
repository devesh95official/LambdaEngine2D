#pragma once
// Toast notifications. Fixed capacity; duplicates merge into one
// toast with a count, and when full the oldest is dropped.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <lam/core/fixed_string.h>
#include <lam/math/color.h>
namespace lam {
// Short notifications at the bottom of the screen (App resource of UiPlugin).
class ToastQueue {
public:
    // remaining: seconds left; alpha: 0..1 (fades in over 0.15 s, out over the last 0.4 s);
    // count: identical pushes merged into this toast; age: seconds since it (re)appeared.
    struct Toast { FixedString<64> text; Color color = kWhite; float remaining = 0; float alpha = 1; std::uint16_t count = 1; float age = 0; };
    explicit ToastQueue(std::size_t capacity = 8);              // capacity >= 1
    // Shows `text` for `seconds` (at least 0.6). If the same text is already showing, that toast
    // restarts and its count goes up instead.
    void push(std::string_view text, float seconds = 2.5f, Color c = kWhite);
    void update(float dt);                                      // real seconds; dt <= 0 does nothing
    std::span<const Toast> toasts() const;                      // oldest first; valid until push/update
    void clear();                                               // removes every toast
    std::size_t capacity() const;                               // toasts shown at once
private:
    std::vector<Toast> toasts_;
    std::size_t capacity_;
};
}
