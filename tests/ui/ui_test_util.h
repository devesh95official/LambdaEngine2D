#pragma once
// Shared lam_ui test helpers. Layout is 480x270 with the built-in theme and 6x8 fallback metrics.
#include <lam/math/rect.h>
#include <lam/ui/ui.h>

namespace lam::uitest {

inline constexpr Rect kScreen{0, 0, 480, 270};

inline void lay(UiTree &t, const Font *font = nullptr) {
    t.layout(kScreen, default_ui_theme(), font);
}

// One frame: layout, input, layout again when input changed something (as ui_update does).
inline void frame(UiTree &t, const UiInput &in, const Font *font = nullptr) {
    t.clear_events();
    lay(t, font);
    t.handle_input(in);
    if (t.layout_dirty()) lay(t, font);
}

inline UiInput key_up() { UiInput i; i.up = true; return i; }
inline UiInput key_down() { UiInput i; i.down = true; return i; }
inline UiInput key_left() { UiInput i; i.left = true; return i; }
inline UiInput key_right() { UiInput i; i.right = true; return i; }
inline UiInput key_confirm() { UiInput i; i.confirm = true; return i; }
inline UiInput key_back() { UiInput i; i.back = true; return i; }

inline UiInput pointer_at(Vec2 p, bool down = false, bool pressed = false, bool released = false) {
    UiInput i;
    i.pointer = p;
    i.pointer_valid = true;
    i.pointer_down = down;
    i.pointer_pressed = pressed;
    i.pointer_released = released;
    return i;
}

inline Vec2 center_of(const Rect &r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }

// Press and release on `p` over two frames (a click).
inline void click(UiTree &t, Vec2 p) {
    frame(t, pointer_at(p, true, true, false));
    frame(t, pointer_at(p, false, false, true));
}

inline int count_events(const UiTree &t, UiEvent::Kind kind) {
    int n = 0;
    for (const UiEvent &e : t.events())
        if (e.kind == kind) ++n;
    return n;
}

} // namespace lam::uitest
