// raylib input backend. Also reads the key queue so taps shorter than a frame still count.
// No `using namespace lam`: raylib has its own global MouseButton, Color, etc.
#include <raylib.h>

#include <algorithm>
#include <array>

#include <lam/core/app.h>
#include <lam/input/driver.h>
#include <lam/input/input_plugin.h>
#include <lam/input/keys.h>
#include <lam/input/raw_input.h>

namespace {

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
#define LAM_INPUT_REAL_TOUCH 1
// Touch ids from last frame, used to set `began`.
std::array<int, lam::kMaxTouches> g_prev_touch_ids{};
int g_prev_touch_count = 0;
#endif

void raw_input_poll(lam::Ctx &ctx) {
    lam::InputSystem *sys = ctx.try_res<lam::InputSystem>();
    if (sys == nullptr || !IsWindowReady()) return;
    lam::RawInput r{};

    for (int k = 1; k < lam::kKeyCount; ++k)
        if (IsKeyDown(k)) r.set_key(static_cast<lam::Key>(k), true);
    for (int k = GetKeyPressed(); k != 0; k = GetKeyPressed())
        r.set_key(static_cast<lam::Key>(k), true);

    const int screen_w = GetScreenWidth();
    const int screen_h = GetScreenHeight();
    const float sx = screen_w > 0 ? static_cast<float>(GetRenderWidth()) / static_cast<float>(screen_w) : 1.0f;
    const float sy = screen_h > 0 ? static_cast<float>(GetRenderHeight()) / static_cast<float>(screen_h) : 1.0f;

    for (int b = 0; b <= MOUSE_BUTTON_BACK; ++b)
        if (IsMouseButtonDown(b) || IsMouseButtonPressed(b)) r.mouse_down |= static_cast<std::uint8_t>(1u << b);
    const ::Vector2 mouse = GetMousePosition();
    r.mouse_fb = lam::Vec2{mouse.x * sx, mouse.y * sy};
    r.mouse_valid = IsCursorOnScreen();
    r.wheel = GetMouseWheelMove();

    for (int p = 0; p < lam::kMaxGamepads; ++p) {
        lam::RawInput::Pad &pad = r.pads[static_cast<std::size_t>(p)];
        pad.connected = IsGamepadAvailable(p);
        if (!pad.connected) continue;
        for (int b = 1; b < lam::kPadButtonCount; ++b)
            if (IsGamepadButtonDown(p, b)) pad.buttons |= 1u << b;
        for (int a = 0; a < lam::kPadAxisCount; ++a) {
            float v = GetGamepadAxisMovement(p, a);
            if (a == GAMEPAD_AXIS_LEFT_TRIGGER || a == GAMEPAD_AXIS_RIGHT_TRIGGER) v = (v + 1.0f) * 0.5f;
            pad.axes[static_cast<std::size_t>(a)] = std::clamp(v, -1.0f, 1.0f);
        }
    }

#if defined(LAM_INPUT_REAL_TOUCH)
    {
        const int n = std::min(GetTouchPointCount(), lam::kMaxTouches);
        for (int i = 0; i < n; ++i) {
            lam::RawTouch &t = r.touches[static_cast<std::size_t>(i)];
            t.id = GetTouchPointId(i);
            const ::Vector2 pos = GetTouchPosition(i);
            t.pos_fb = lam::Vec2{pos.x * sx, pos.y * sy};
            t.began = std::find(g_prev_touch_ids.begin(), g_prev_touch_ids.begin() + g_prev_touch_count, t.id) ==
                      g_prev_touch_ids.begin() + g_prev_touch_count;
        }
        r.touch_count = n;
        g_prev_touch_count = n;
        for (int i = 0; i < n; ++i)
            g_prev_touch_ids[static_cast<std::size_t>(i)] = r.touches[static_cast<std::size_t>(i)].id;
    }
#endif

    for (int c = GetCharPressed(); c != 0; c = GetCharPressed())
        if (r.text_count < lam::kMaxTextPerFrame) r.text[static_cast<std::size_t>(r.text_count++)] = static_cast<char32_t>(c);

    sys->feed_raw(r);
}

} // namespace

void lam::RaylibInputPlugin::build(lam::App &app) {
    app.require_plugin("lam.input", name());
    lam::SystemDesc d;
    d.stage = lam::Stage::FrameBegin;
    d.fn = &raw_input_poll;
    d.name = "raw_input_poll";
    d.order = -5000;
    d.scope = lam::SystemScope::Once;
    app.add_system(d);
}
