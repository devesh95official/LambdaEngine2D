// InputSystem. A frame can run 0..N ticks, so presses are latched and handed to the next tick
// that runs; held state and axes are shared by all ticks of the frame.
#include <lam/input/driver.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>

#include <lam/core/assert.h>
#include <lam/render/camera.h>
#include <lam/render/virtual_screen.h>

#include "detail/input_state.h"

namespace lam {
namespace {

using Live = detail::InputAccess::SysImpl;
using MapImpl = detail::InputAccess::MapImpl;

constexpr std::uint8_t kLeftBit = 1;                            // MouseButton::Left; touch 0
constexpr float kRepeatEpsilon = 1e-4f;                         // seconds; absorbs float drift

void set_bit(ActionBits &b, std::size_t id) {
    b[id >> 6] |= std::uint64_t{1} << (id & 63u);
}
bool get_bit(const ActionBits &b, std::size_t id) {
    return ((b[id >> 6] >> (id & 63u)) & 1u) != 0;
}
ActionBits and_not(const ActionBits &a, const ActionBits &b) {
    return {a[0] & ~b[0], a[1] & ~b[1], a[2] & ~b[2], a[3] & ~b[3]};
}
void or_into(ActionBits &dst, const ActionBits &src) {
    for (std::size_t i = 0; i < dst.size(); ++i)
        dst[i] |= src[i];
}

std::optional<Vec2> to_virtual(const VirtualLayout *layout, Vec2 fb) {
    if (!std::isfinite(fb.x) || !std::isfinite(fb.y)) return std::nullopt;
    if (layout == nullptr || layout->framebuffer.x <= 0 || layout->framebuffer.y <= 0) return fb;
    return framebuffer_to_virtual(*layout, fb);
}

std::uint8_t player_pad_mask(const MapImpl &m, int player) {
    const int pad = m.pad[static_cast<std::size_t>(player)];
    if (pad == kAnyPad) return 0x0F;
    if (pad >= 0 && pad < kMaxGamepads) return static_cast<std::uint8_t>(1u << pad);
    return 0;
}

std::uint8_t binding_pad_mask(std::int8_t pad, std::uint8_t player_mask) {
    if (pad >= 0 && pad < kMaxGamepads) return static_cast<std::uint8_t>(1u << pad);
    return pad == -1 ? player_mask : std::uint8_t{0};
}

struct Eval {
    const RawInput &raw;
    std::uint8_t connected;                                     // pad bits
    std::uint8_t pointer_down;                                  // mouse | touch 0 as Left
    const std::array<ZoneLive, kMaxTouchZones> &zones;
    int zone_count;
};

// Largest value of pad axis `a` in direction `positive` over the pads in `mask`.
float pad_axis_value(const Eval &e, std::uint8_t mask, std::uint16_t a, bool positive) {
    float best = 0;
    if (a >= kPadAxisCount) return 0;
    mask &= e.connected;
    for (int p = 0; p < kMaxGamepads; ++p) {
        if ((mask & (1u << p)) == 0) continue;
        const float v = e.raw.pads[static_cast<std::size_t>(p)].axes[a];
        const float d = positive ? v : -v;
        if (std::isfinite(d) && d > best) best = d;
    }
    return best;
}

// [0, 1]; > 0 is down.
float binding_value(const Eval &e, const Binding &b, std::uint8_t player_mask, bool keyboard_mouse) {
    switch (b.source) {
    case BindingSource::None: return 0;
    case BindingSource::Key: return keyboard_mouse && e.raw.key(static_cast<Key>(b.code)) ? 1.0f : 0.0f;
    case BindingSource::Mouse:
        return keyboard_mouse && b.code < 8 && ((e.pointer_down >> b.code) & 1u) != 0 ? 1.0f : 0.0f;
    case BindingSource::PadButton: {
        if (b.code >= 32) return 0;
        const std::uint8_t mask = binding_pad_mask(b.pad, player_mask) & e.connected;
        for (int p = 0; p < kMaxGamepads; ++p)
            if ((mask & (1u << p)) != 0 && ((e.raw.pads[static_cast<std::size_t>(p)].buttons >> b.code) & 1u) != 0)
                return 1;
        return 0;
    }
    case BindingSource::PadAxisPositive:
    case BindingSource::PadAxisNegative: {
        const float v = pad_axis_value(e, binding_pad_mask(b.pad, player_mask), b.code,
                                       b.source == BindingSource::PadAxisPositive);
        const float t = std::clamp(std::isfinite(b.threshold) ? b.threshold : 0.5f, 0.0f, 0.99f);
        if (!(v > t)) return 0;
        return std::min(1.0f, (v - t) / (1.0f - t));
    }
    case BindingSource::WheelUp: return keyboard_mouse && e.raw.wheel > 0 ? 1.0f : 0.0f;
    case BindingSource::WheelDown: return keyboard_mouse && e.raw.wheel < 0 ? 1.0f : 0.0f;
    case BindingSource::TouchZone:
        return b.code < e.zone_count && e.zones[b.code].held ? 1.0f : 0.0f;
    }
    return 0;
}

// Below dz reads 0; above, rescale to 0..1.
Vec2 apply_deadzone(Vec2 v, float dz) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y)) return {};
    dz = std::clamp(std::isfinite(dz) ? dz : 0.2f, 0.0f, 0.99f);
    const float len = length(v);
    if (len <= dz || len <= 0) return {};
    const float scaled = std::min(1.0f, (len - dz) / (1.0f - dz));
    return v * (scaled / len);
}

Vec2 eval_axis(const Eval &e, const AxisSlots &s, bool two_d, std::uint8_t player_mask, bool keyboard_mouse) {
    Vec2 keys{};
    for (std::size_t i = 0; i < s.key_count; ++i) {
        const Axis2Binding &k = s.keys[i];
        keys.x += binding_value(e, k.right, player_mask, keyboard_mouse) -
                  binding_value(e, k.left, player_mask, keyboard_mouse);
        if (two_d)
            keys.y += binding_value(e, k.down, player_mask, keyboard_mouse) -
                      binding_value(e, k.up, player_mask, keyboard_mouse);
    }
    if (two_d) {
        if (length_sq(keys) > 1.0f) keys = keys / length(keys);   // diagonals: length 1
    } else {
        keys.x = std::clamp(keys.x, -1.0f, 1.0f);
    }
    Vec2 total = keys;
    for (std::size_t i = 0; i < s.stick_count; ++i) {
        const StickBinding &st = s.sticks[i];
        if (st.pad == kNoPad) continue;
        const std::uint8_t mask = binding_pad_mask(st.pad, player_mask) & e.connected;
        const auto xi = static_cast<std::size_t>(st.x), yi = static_cast<std::size_t>(st.y);
        if (xi >= kPadAxisCount || yi >= kPadAxisCount) continue;
        for (int p = 0; p < kMaxGamepads; ++p) {
            if ((mask & (1u << p)) == 0) continue;
            const auto &axes = e.raw.pads[static_cast<std::size_t>(p)].axes;
            total += apply_deadzone(Vec2{axes[xi], two_d ? axes[yi] : 0.0f}, st.deadzone);
        }
    }
    if (two_d) return clamp_length(total, 1.0f);
    return Vec2{std::clamp(total.x, -1.0f, 1.0f), 0.0f};
}

// clear_touch_zones() drops every capture, so a finger still on an old zone doesn't end up
// holding the new zone that reused its index.
void update_zones(Live &s, const MapImpl &m, const VirtualLayout *layout, const RawInput &raw, int touches) {
    if (s.seen_zone_layout != m.zone_layout) {
        s.seen_zone_layout = m.zone_layout;
        for (ZoneLive &zl : s.zones)
            zl = ZoneLive{};
    }
    for (int z = 0; z < kMaxTouchZones; ++z) {
        ZoneLive &zl = s.zones[static_cast<std::size_t>(z)];
        if (zl.touch_id < 0) continue;
        const RawTouch *found = nullptr;
        for (int i = 0; i < touches; ++i)
            if (raw.touches[static_cast<std::size_t>(i)].id == zl.touch_id) found = &raw.touches[static_cast<std::size_t>(i)];
        if (found == nullptr || z >= m.zone_count) {
            zl = ZoneLive{};
            continue;
        }
        if (auto v = to_virtual(layout, found->pos_fb)) zl.pos = *v;
        zl.held = true;
    }
    for (int i = 0; i < touches; ++i) {
        const RawTouch &t = raw.touches[static_cast<std::size_t>(i)];
        if (t.id < 0) continue;
        bool is_new = t.began;
        if (!is_new) {
            is_new = true;
            for (int j = 0; j < s.prev_touch_count; ++j)
                if (s.prev_touch_ids[static_cast<std::size_t>(j)] == t.id) is_new = false;
        }
        if (!is_new) continue;
        bool taken = false;
        for (const ZoneLive &zl : s.zones)
            taken = taken || zl.touch_id == t.id;
        if (taken) continue;
        const auto v = to_virtual(layout, t.pos_fb);
        if (!v) continue;
        for (int z = 0; z < m.zone_count; ++z) {
            ZoneLive &zl = s.zones[static_cast<std::size_t>(z)];
            if (zl.touch_id >= 0 || !m.zones[static_cast<std::size_t>(z)].virtual_rect.contains(*v)) continue;
            zl.touch_id = t.id;
            zl.origin = *v;
            zl.pos = *v;
            zl.held = true;
            break;
        }
    }
    s.prev_touch_count = touches;
    for (int i = 0; i < touches; ++i)
        s.prev_touch_ids[static_cast<std::size_t>(i)] = raw.touches[static_cast<std::size_t>(i)].id;
}

Vec2 zone_stick(const ZoneLive &zl, const TouchZone &z) {
    const float radius = 0.5f * std::min(z.virtual_rect.w, z.virtual_rect.h);
    if (!zl.held || !(radius > 0)) return {};
    return clamp_length((zl.pos - zl.origin) / radius, 1.0f);
}

// First new press of any device this frame. Escape means cancel.
std::optional<Binding> capture_press(const Live &s, const RawInput &raw, std::uint8_t connected, bool &cancel) {
    const RawInput &prev = s.prev_raw;
    cancel = raw.key(Key::Escape) && !prev.key(Key::Escape);
    if (cancel) return std::nullopt;
    for (std::size_t w = 0; w < raw.keys_down.size(); ++w) {
        std::uint64_t edge = raw.keys_down[w] & ~prev.keys_down[w];
        while (edge != 0) {
            const int bit = std::countr_zero(edge);
            edge &= edge - 1;
            const int code = static_cast<int>(w) * 64 + bit;
            if (code > 0 && code < kKeyCount) return ActionMap::key(static_cast<Key>(code));
        }
    }
    if (const std::uint8_t edge = raw.mouse_down & static_cast<std::uint8_t>(~prev.mouse_down); edge != 0)
        return ActionMap::mouse(static_cast<MouseButton>(std::countr_zero(static_cast<unsigned>(edge))));
    if (raw.wheel != 0 && std::isfinite(raw.wheel)) return ActionMap::wheel(raw.wheel > 0);
    for (int p = 0; p < kMaxGamepads; ++p) {
        if ((connected & (1u << p)) == 0) continue;
        const RawInput::Pad &pad = raw.pads[static_cast<std::size_t>(p)];
        const RawInput::Pad &old = prev.pads[static_cast<std::size_t>(p)];
        std::uint32_t edge = pad.buttons & ~(old.connected ? old.buttons : 0u) & ~1u;   // bit 0 = Unknown
        if (edge != 0 && std::countr_zero(edge) < kPadButtonCount)
            return ActionMap::pad(static_cast<PadButton>(std::countr_zero(edge)));
        for (int a = 0; a < kPadAxisCount; ++a) {
            const float v = pad.axes[static_cast<std::size_t>(a)];
            const float pv = old.connected ? old.axes[static_cast<std::size_t>(a)] : 0.0f;
            if (v > 0.5f && !(pv > 0.5f)) return ActionMap::pad_axis(static_cast<PadAxis>(a), true);
            if (v < -0.5f && !(pv < -0.5f)) return ActionMap::pad_axis(static_cast<PadAxis>(a), false);
        }
    }
    return std::nullopt;
}

void process_capture(Live &s, MapImpl &m, const RawInput &raw, std::uint8_t connected) {
    bool cancel = false;
    const std::optional<Binding> got = capture_press(s, raw, connected, cancel);
    if (cancel) {
        m.capture = CaptureKind::None;
        return;
    }
    if (!got) return;
    const auto player = static_cast<std::size_t>(m.capture_player);
    const auto slot = static_cast<std::size_t>(m.capture_slot);
    if (m.capture == CaptureKind::Button) {
        ButtonSlots &bs = m.buttons[m.capture_id][player];
        bs.slot[slot] = *got;
        detail::recount(bs);
    } else {
        AxisSlots &as = m.axes[m.capture_id][player];
        detail::dir_ref(as.keys[slot], m.capture_dir) = *got;
        detail::recount(as);
    }
    m.capture = CaptureKind::None;
    ++m.revision;
}

} // namespace

// ---- InputSystem API ---------------------------------------------------------------------------

InputSystem::InputSystem() : impl_(std::make_unique<Impl>()) {}
InputSystem::~InputSystem() = default;

void InputSystem::set_driver(InputDriver *driver) {
    impl_->driver = driver;
}
InputDriver *InputSystem::driver() const {
    return impl_->driver;
}
void InputSystem::set_recorder(InputRecorder *recorder) {
    impl_->recorder = recorder;
}
InputRecorder *InputSystem::recorder() const {
    return impl_->recorder;
}

void InputSystem::feed_raw(const RawInput &r) {
    impl_->raw = r;
    impl_->fed = true;
}

const RawInput &InputSystem::raw() const {
    return impl_->raw;
}

std::span<const char32_t> InputSystem::text_typed() const {
    return {impl_->text.data(), static_cast<std::size_t>(impl_->text_count)};
}

void InputSystem::queue_intent(const Intent &intent, int player) {
    LAM_ASSERT(player >= 0 && player < kMaxPlayers, "InputSystem::queue_intent: player out of range");
    Impl &s = *impl_;
    if (player < 0 || player >= kMaxPlayers) {
        ++s.dropped;
        return;
    }
    const auto p = static_cast<std::size_t>(player);
    if (s.intent_count[p] >= kMaxIntents) {
        ++s.dropped;
        return;
    }
    s.intents[p][s.intent_count[p]++] = intent;
}

std::uint32_t InputSystem::dropped_intents() const {
    return impl_->dropped;
}

void InputSystem::reset_latch() {
    Impl &s = *impl_;
    for (PlayerLive &pl : s.players) {
        pl.latch_pressed = {};
        pl.latch_released = {};
    }
    s.latch_pointer_pressed = 0;
    s.latch_wheel = 0;
}

// ---- Frame and tick paths ----------------------------------------------------------------------

namespace detail {

Vec2 to_world(const CameraView *view, Vec2 virtual_pos) {
    if (view == nullptr || !view->valid) return virtual_pos;
    return screen_to_world(*view, virtual_pos);
}

void input_frame_update(InputSystem &sys, ActionMap &map, FrameInput &out, const FrameEnv &env) {
    Live &s = InputAccess::sys(sys);
    MapImpl &m = InputAccess::map(map);
    RawInput &raw = s.raw;
    if (!s.fed) {                                               // no new feed: drop the deltas
        raw.wheel = 0;
        raw.text_count = 0;
        for (RawTouch &t : raw.touches)
            t.began = false;
    }
    s.fed = false;
    if (!std::isfinite(raw.wheel)) raw.wheel = 0;
    s.text_count = std::clamp(raw.text_count, 0, kMaxTextPerFrame);
    std::copy_n(raw.text.begin(), s.text_count, s.text.begin());

    // Reset the latch on pause, focus change or losing the window.
    bool reset = env.lost_focus;
    if (env.paused != s.last_paused) {
        s.last_paused = env.paused;
        reset = true;
    }
    if (env.focus_id != s.focus_id) {
        s.focus_id = env.focus_id;
        reset = true;
    }
    if (reset) sys.reset_latch();

    std::uint8_t connected = 0;
    for (int p = 0; p < kMaxGamepads; ++p)
        if (raw.pads[static_cast<std::size_t>(p)].connected) connected |= static_cast<std::uint8_t>(1u << p);

    // Pointer: touch 0 when touches exist, else the mouse.
    const int touches = std::clamp(raw.touch_count, 0, kMaxTouches);
    std::optional<Vec2> fb;
    if (touches > 0) fb = raw.touches[0].pos_fb;
    else if (raw.mouse_valid) fb = raw.mouse_fb;
    const std::uint8_t pdown = static_cast<std::uint8_t>(raw.mouse_down | (touches > 0 ? kLeftBit : 0));
    s.pointer_valid = false;
    if (fb) {
        if (const auto v = to_virtual(env.layout, *fb)) {
            s.pointer_virtual = *v;
            s.pointer_valid = true;
        }
    }
    // While capturing, swallow this frame's presses, clicks, wheel and text so the captured
    // input doesn't also trigger something. The UI can't rely on capturing() here: it's already
    // false by Update.
    const bool suppress = m.capture != CaptureKind::None;
    auto ppressed = static_cast<std::uint8_t>(pdown & ~s.prev_pointer_down);
    auto preleased = static_cast<std::uint8_t>(~pdown & s.prev_pointer_down);
    const float wheel = suppress ? 0.0f : raw.wheel;
    if (suppress) {
        ppressed = 0;
        preleased = 0;
        s.text_count = 0;
    }
    s.prev_pointer_down = pdown;
    s.pointer_down = pdown;
    s.latch_pointer_pressed |= ppressed;
    s.latch_wheel += wheel;

    update_zones(s, m, env.layout, raw, touches);

    if (suppress) process_capture(s, m, raw, connected);

    const Eval e{raw, connected, pdown, s.zones, m.zone_count};
    const std::size_t buttons = m.button_names.size();
    const std::size_t axes = m.axis_names.size();
    for (int p = 0; p < kMaxPlayers; ++p) {
        const auto pi = static_cast<std::size_t>(p);
        PlayerLive &pl = s.players[pi];
        const bool kbm = m.keyboard_mouse[pi];
        const std::uint8_t pmask = player_pad_mask(m, p);
        ActionBits down{};
        ActionBits wheel_only{};                                // down only through wheel bindings
        for (std::size_t a = 1; a < buttons; ++a) {
            const ButtonSlots &bs = m.buttons[a][pi];
            bool any = false, wheel_hit = false;
            for (std::size_t i = 0; i < bs.count; ++i) {
                if (!(binding_value(e, bs.slot[i], pmask, kbm) > 0)) continue;
                const BindingSource src = bs.slot[i].source;
                if (src == BindingSource::WheelUp || src == BindingSource::WheelDown) {
                    wheel_hit = true;
                    continue;
                }
                any = true;
                break;
            }
            if (any || wheel_hit) set_bit(down, a);
            if (wheel_hit && !any) set_bit(wheel_only, a);
        }
        std::array<Vec2, kMaxAxisActions> values{};
        for (std::size_t a = 1; a < axes; ++a)
            values[a] = eval_axis(e, m.axes[a][pi], m.axis_2d[a], pmask, kbm);
        if (p == 0) {
            for (int z = 0; z < m.zone_count; ++z) {
                const TouchZone &tz = m.zones[static_cast<std::size_t>(z)];
                const ZoneLive &zl = s.zones[static_cast<std::size_t>(z)];
                if (!zl.held) continue;
                if (tz.button.id != 0 && tz.button.id < buttons) {
                    set_bit(down, tz.button.id);
                    wheel_only[tz.button.id >> 6] &= ~(std::uint64_t{1} << (tz.button.id & 63u));
                }
                if (tz.stick.id != 0 && tz.stick.id < axes) {
                    Vec2 &v = values[tz.stick.id];
                    v = v + zone_stick(zl, tz);
                    v = m.axis_2d[tz.stick.id] ? clamp_length(v, 1.0f) : Vec2{std::clamp(v.x, -1.0f, 1.0f), 0.0f};
                }
            }
        }
        // Treat wheel-only actions as released last frame, so continuous scrolling keeps pressing.
        const ActionBits edge = and_not(down, and_not(pl.prev_down, pl.wheel_only));
        const ActionBits released = and_not(pl.prev_down, down);
        const ActionBits pressed = suppress ? ActionBits{} : edge;
        or_into(pl.latch_pressed, pressed);
        or_into(pl.latch_released, released);
        pl.prev_down = down;
        pl.wheel_only = wheel_only;
        pl.down = down;
        pl.axes = values;
        if (p != 0) continue;

        // FrameInput is player 0 only, with repeat in real time.
        ActionBits repeated = pressed;
        const float tick_dt = env.tick_dt > 0 ? env.tick_dt : 1.0f / 60;
        const float frame_dt = std::isfinite(env.frame_dt) ? std::max(0.0f, env.frame_dt) : 0.0f;
        for (std::size_t a = 1; a < buttons; ++a) {
            const std::uint16_t interval = m.repeat_interval[a];
            if (interval == 0) continue;
            const float interval_s = static_cast<float>(interval) * tick_dt;
            const std::uint16_t delay = m.repeat_delay[a];
            if (get_bit(edge, a)) { // restart the delay, even for a press the capture swallowed
                s.held_seconds[a] = 0;
                s.next_repeat[a] = delay > 0 ? static_cast<float>(delay) * tick_dt : interval_s;
            } else if (get_bit(down, a)) {
                s.held_seconds[a] += frame_dt;
                if (s.held_seconds[a] + kRepeatEpsilon >= s.next_repeat[a]) {
                    set_bit(repeated, a);
                    while (s.next_repeat[a] <= s.held_seconds[a] + kRepeatEpsilon)
                        s.next_repeat[a] += interval_s;
                }
            } else {
                s.held_seconds[a] = 0;
            }
        }
        out.pressed = pressed;
        out.held = down;
        out.released = released;
        out.repeated = repeated;
        out.axes = values;
    }

    PointerState &ps = out.pointer;
    ps.virtual_pos = s.pointer_virtual;
    ps.world = to_world(env.view, s.pointer_virtual);
    ps.valid = s.pointer_valid;
    ps.down = pdown;
    ps.pressed = ppressed;
    ps.released = preleased;
    ps.wheel = wheel;
    s.prev_raw = raw;
}

void input_publish(InputSystem &sys, const ActionMap &map, ActionState &state, const CameraView *view,
                   std::uint32_t focus_id) {
    Live &s = InputAccess::sys(sys);
    const MapImpl &m = InputAccess::map(map);
    if (focus_id != s.focus_id) {                               // world stack changed since FrameBegin
        s.focus_id = focus_id;
        sys.reset_latch();
    }
    const Vec2 world = to_world(view, s.pointer_virtual);
    const std::size_t buttons = m.button_names.size();
    for (int p = 0; p < kMaxPlayers; ++p) {
        const auto pi = static_cast<std::size_t>(p);
        PlayerLive &pl = s.players[pi];
        InputFrame &f = state.players[pi];
        f = InputFrame{};
        f.pressed = pl.latch_pressed;
        f.released = pl.latch_released;
        f.held = pl.down;
        pl.latch_pressed = {};
        pl.latch_released = {};
        ActionBits repeated = f.pressed;
        for (std::size_t a = 1; a < buttons; ++a) {
            const std::uint16_t interval = m.repeat_interval[a];
            if (interval == 0) continue;
            std::uint32_t &t = pl.held_ticks[a];
            if (get_bit(f.pressed, a)) {
                t = 0;
            } else if (get_bit(f.held, a)) {
                ++t;
                const std::uint32_t delay = m.repeat_delay[a];
                const std::uint32_t first = delay > 0 ? delay : interval;
                if (t >= first && (t - first) % interval == 0) set_bit(repeated, a);
            } else {
                t = 0;
            }
        }
        f.repeated = repeated;
        f.axes = pl.axes;
        if (m.keyboard_mouse[pi]) {
            f.pointer_virtual = s.pointer_virtual;
            f.pointer_world = world;
            f.pointer_valid = s.pointer_valid;
            f.pointer_down = s.pointer_down;
            f.pointer_pressed = s.latch_pointer_pressed;
            f.wheel = s.latch_wheel;
        }
        const std::uint8_t n = s.intent_count[pi];
        for (std::uint8_t i = 0; i < n; ++i)
            f.intents[i] = s.intents[pi][i];
        f.intent_count = n;
        s.intent_count[pi] = 0;
    }
    s.latch_pointer_pressed = 0;
    s.latch_wheel = 0;
}

void canonical_frame_bytes(const InputFrame &f, std::byte *out) {
    std::memset(out, 0, sizeof(InputFrame));
    const auto put = [out](std::size_t offset, const void *src, std::size_t size) {
        std::memcpy(out + offset, src, size);
    };
    put(offsetof(InputFrame, pressed), &f.pressed, sizeof f.pressed);
    put(offsetof(InputFrame, held), &f.held, sizeof f.held);
    put(offsetof(InputFrame, released), &f.released, sizeof f.released);
    put(offsetof(InputFrame, repeated), &f.repeated, sizeof f.repeated);
    put(offsetof(InputFrame, axes), &f.axes, sizeof f.axes);
    put(offsetof(InputFrame, pointer_virtual), &f.pointer_virtual, sizeof f.pointer_virtual);
    put(offsetof(InputFrame, pointer_world), &f.pointer_world, sizeof f.pointer_world);
    put(offsetof(InputFrame, pointer_valid), &f.pointer_valid, sizeof f.pointer_valid);
    put(offsetof(InputFrame, pointer_down), &f.pointer_down, sizeof f.pointer_down);
    put(offsetof(InputFrame, pointer_pressed), &f.pointer_pressed, sizeof f.pointer_pressed);
    put(offsetof(InputFrame, wheel), &f.wheel, sizeof f.wheel);
    for (std::size_t i = 0; i < f.intents.size(); ++i) {
        const std::size_t base = offsetof(InputFrame, intents) + i * sizeof(Intent);
        put(base + offsetof(Intent, kind), &f.intents[i].kind, sizeof(StrId));
        put(base + offsetof(Intent, a), &f.intents[i].a, sizeof(std::int64_t));
        put(base + offsetof(Intent, b), &f.intents[i].b, sizeof(std::int64_t));
    }
    put(offsetof(InputFrame, intent_count), &f.intent_count, sizeof f.intent_count);
}

} // namespace detail
} // namespace lam
