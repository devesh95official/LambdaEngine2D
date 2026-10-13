// ActionMap. Persistence is in bindings.cpp.
#include <lam/input/actions.h>

#include <algorithm>
#include <deque>
#include <utility>

#include <lam/core/assert.h>
#include <lam/core/log.h>

#include "detail/input_state.h"

namespace lam {
namespace {

bool valid_player(int player) {
    return player >= 0 && player < kMaxPlayers;
}
bool valid_slot(int slot) {
    return slot >= 0 && slot < kMaxBindingSlots;
}

// Empty stick slots are marked with pad == kNoPad (a stick never reads "no pad").
StickBinding empty_stick() {
    StickBinding s;
    s.pad = static_cast<std::int8_t>(kNoPad);
    return s;
}

int find_name(const std::deque<std::string> &names, std::string_view name) {
    for (std::size_t i = 1; i < names.size(); ++i)
        if (names[i] == name) return static_cast<int>(i);
    return 0;
}

} // namespace

namespace detail {

void recount(ButtonSlots &s) {
    std::uint8_t n = 0;
    for (int i = 0; i < kMaxBindingSlots; ++i)
        if (s.slot[static_cast<std::size_t>(i)].source != BindingSource::None)
            n = static_cast<std::uint8_t>(i + 1);
    s.count = n;
}

void recount(AxisSlots &s) {
    std::uint8_t n = 0;
    for (int i = 0; i < kMaxBindingSlots; ++i) {
        const Axis2Binding &k = s.keys[static_cast<std::size_t>(i)];
        if (k.up.source != BindingSource::None || k.down.source != BindingSource::None ||
            k.left.source != BindingSource::None || k.right.source != BindingSource::None)
            n = static_cast<std::uint8_t>(i + 1);
    }
    s.key_count = n;
    n = 0;
    for (int i = 0; i < kMaxBindingSlots; ++i)
        if (s.sticks[static_cast<std::size_t>(i)].pad != kNoPad) n = static_cast<std::uint8_t>(i + 1);
    s.stick_count = n;
}

Binding &dir_ref(Axis2Binding &b, AxisDir dir) {
    switch (dir) {
    case AxisDir::Up: return b.up;
    case AxisDir::Down: return b.down;
    case AxisDir::Left: return b.left;
    case AxisDir::Right: return b.right;
    }
    return b.up;
}

} // namespace detail

using detail::dir_ref;
using detail::recount;

ActionMap::ActionMap() : impl_(std::make_unique<Impl>()) {
    for (auto &per_player : impl_->axes)
        for (AxisSlots &s : per_player)
            s.sticks.fill(empty_stick());
    for (AxisSlots &s : impl_->axis_defaults)
        s.sticks.fill(empty_stick());
}
ActionMap::~ActionMap() = default;
ActionMap::ActionMap(const ActionMap &other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
ActionMap &ActionMap::operator=(const ActionMap &other) {
    if (this != &other) *impl_ = *other.impl_;
    return *this;
}
ActionMap::ActionMap(ActionMap &&) noexcept = default;
ActionMap &ActionMap::operator=(ActionMap &&) noexcept = default;

// ---- Definitions -------------------------------------------------------------------------------

ButtonAction ActionMap::button(std::string_view name, std::initializer_list<Binding> defaults) {
    Impl &m = *impl_;
    if (const int id = find_name(m.button_names, name); id != 0)
        return ButtonAction{static_cast<std::uint16_t>(id)};
    LAM_ASSERT(!name.empty(), "ActionMap::button: empty name");
    LAM_ASSERT(find_name(m.axis_names, name) == 0, "ActionMap::button: name already used by an axis");
    LAM_ASSERT(m.button_names.size() < static_cast<std::size_t>(kMaxButtonActions),
               "ActionMap::button: more than 255 button actions");
    LAM_ASSERT(defaults.size() <= static_cast<std::size_t>(kMaxBindingSlots),
               "ActionMap::button: more defaults than kMaxBindingSlots");
    if (name.empty() || find_name(m.axis_names, name) != 0 ||
        m.button_names.size() >= static_cast<std::size_t>(kMaxButtonActions))
        return {};
    const auto id = static_cast<std::uint16_t>(m.button_names.size());
    m.button_names.emplace_back(name);
    ButtonSlots &slots = m.button_defaults[id];
    int i = 0;
    for (const Binding &b : defaults) {
        if (i >= kMaxBindingSlots) break;
        slots.slot[static_cast<std::size_t>(i++)] = b;
    }
    recount(slots);
    m.buttons[id][0] = slots;
    ++m.revision;
    return ButtonAction{id};
}

namespace {
AxisAction define_axis(detail::InputAccess::MapImpl &m, std::string_view name, bool two_d, AxisSlots slots) {
    LAM_ASSERT(!name.empty(), "ActionMap: empty axis name");
    LAM_ASSERT(find_name(m.button_names, name) == 0, "ActionMap: axis name already used by a button");
    LAM_ASSERT(m.axis_names.size() < static_cast<std::size_t>(kMaxAxisActions),
               "ActionMap: more than 15 axis actions");
    if (name.empty() || find_name(m.button_names, name) != 0 ||
        m.axis_names.size() >= static_cast<std::size_t>(kMaxAxisActions))
        return {};
    const auto id = static_cast<std::uint8_t>(m.axis_names.size());
    m.axis_names.emplace_back(name);
    m.axis_2d[id] = two_d;
    recount(slots);
    m.axis_defaults[id] = slots;
    m.axes[id][0] = slots;
    ++m.revision;
    return AxisAction{id};
}
} // namespace

AxisAction ActionMap::axis2(std::string_view name, std::initializer_list<Axis2Binding> keys,
                            std::initializer_list<StickBinding> sticks) {
    Impl &m = *impl_;
    if (const int id = find_name(m.axis_names, name); id != 0) return AxisAction{static_cast<std::uint8_t>(id)};
    LAM_ASSERT(keys.size() <= static_cast<std::size_t>(kMaxBindingSlots) &&
                   sticks.size() <= static_cast<std::size_t>(kMaxBindingSlots),
               "ActionMap::axis2: more bindings than kMaxBindingSlots");
    AxisSlots slots;
    slots.sticks.fill(empty_stick());
    int i = 0;
    for (const Axis2Binding &k : keys)
        if (i < kMaxBindingSlots) slots.keys[static_cast<std::size_t>(i++)] = k;
    i = 0;
    for (const StickBinding &s : sticks)
        if (i < kMaxBindingSlots) slots.sticks[static_cast<std::size_t>(i++)] = s;
    return define_axis(m, name, true, slots);
}

AxisAction ActionMap::axis1(std::string_view name, std::initializer_list<Binding> positive,
                            std::initializer_list<Binding> negative) {
    Impl &m = *impl_;
    if (const int id = find_name(m.axis_names, name); id != 0) return AxisAction{static_cast<std::uint8_t>(id)};
    LAM_ASSERT(positive.size() <= static_cast<std::size_t>(kMaxBindingSlots) &&
                   negative.size() <= static_cast<std::size_t>(kMaxBindingSlots),
               "ActionMap::axis1: more bindings than kMaxBindingSlots");
    AxisSlots slots;
    slots.sticks.fill(empty_stick());
    int i = 0;
    for (const Binding &b : positive)
        if (i < kMaxBindingSlots) slots.keys[static_cast<std::size_t>(i++)].right = b;
    i = 0;
    for (const Binding &b : negative)
        if (i < kMaxBindingSlots) slots.keys[static_cast<std::size_t>(i++)].left = b;
    return define_axis(m, name, false, slots);
}

ButtonAction ActionMap::find_button(std::string_view name) const {
    return ButtonAction{static_cast<std::uint16_t>(find_name(impl_->button_names, name))};
}

AxisAction ActionMap::find_axis(std::string_view name) const {
    return AxisAction{static_cast<std::uint8_t>(find_name(impl_->axis_names, name))};
}

std::string_view ActionMap::name_of(ButtonAction a) const {
    return a.id != 0 && a.id < impl_->button_names.size() ? std::string_view(impl_->button_names[a.id])
                                                          : std::string_view{};
}

std::string_view ActionMap::name_of(AxisAction a) const {
    return a.id != 0 && a.id < impl_->axis_names.size() ? std::string_view(impl_->axis_names[a.id])
                                                        : std::string_view{};
}

int ActionMap::button_count() const {
    return static_cast<int>(impl_->button_names.size()) - 1;
}

int ActionMap::axis_count() const {
    return static_cast<int>(impl_->axis_names.size()) - 1;
}

bool ActionMap::is_axis2(AxisAction a) const {
    return a.id != 0 && a.id < impl_->axis_names.size() && impl_->axis_2d[a.id];
}

// ---- Repeat and players ------------------------------------------------------------------------

namespace {
bool known(const detail::InputAccess::MapImpl &m, ButtonAction a) {
    return a.id != 0 && a.id < m.button_names.size();
}
bool known(const detail::InputAccess::MapImpl &m, AxisAction a) {
    return a.id != 0 && a.id < m.axis_names.size();
}
} // namespace

void ActionMap::set_repeat(ButtonAction a, std::uint16_t delay_ticks, std::uint16_t interval_ticks) {
    Impl &m = *impl_;
    LAM_ASSERT(known(m, a), "ActionMap::set_repeat: unknown action");
    if (!known(m, a)) return;
    m.repeat_delay[a.id] = delay_ticks;
    m.repeat_interval[a.id] = interval_ticks;
    ++m.revision;
}

std::uint16_t ActionMap::repeat_delay(ButtonAction a) const {
    return known(*impl_, a) ? impl_->repeat_delay[a.id] : std::uint16_t{0};
}

std::uint16_t ActionMap::repeat_interval(ButtonAction a) const {
    return known(*impl_, a) ? impl_->repeat_interval[a.id] : std::uint16_t{0};
}

void ActionMap::assign_player(int player, int pad, bool keyboard_mouse) {
    Impl &m = *impl_;
    LAM_ASSERT(valid_player(player), "ActionMap::assign_player: player out of range");
    LAM_ASSERT(pad == kAnyPad || pad == kNoPad || (pad >= 0 && pad < kMaxGamepads),
               "ActionMap::assign_player: pad must be 0..3, kAnyPad or kNoPad");
    if (!valid_player(player)) return;
    if (pad != kAnyPad && (pad < 0 || pad >= kMaxGamepads)) pad = kNoPad;
    const auto p = static_cast<std::size_t>(player);
    m.pad[p] = static_cast<std::int8_t>(pad);
    m.keyboard_mouse[p] = keyboard_mouse;
    m.assigned[p] = true;
    ++m.revision;
}

int ActionMap::player_pad(int player) const {
    return valid_player(player) ? impl_->pad[static_cast<std::size_t>(player)] : kNoPad;
}

bool ActionMap::player_keyboard_mouse(int player) const {
    return valid_player(player) && impl_->keyboard_mouse[static_cast<std::size_t>(player)];
}

int ActionMap::player_count() const {
    const Impl &m = *impl_;
    int count = 1;
    for (int p = 1; p < kMaxPlayers; ++p) {
        const auto pi = static_cast<std::size_t>(p);
        bool used = m.assigned[pi];
        for (std::size_t a = 1; !used && a < m.button_names.size(); ++a)
            used = m.buttons[a][pi].count != 0;
        for (std::size_t a = 1; !used && a < m.axis_names.size(); ++a)
            used = m.axes[a][pi].key_count != 0 || m.axes[a][pi].stick_count != 0;
        if (used) count = p + 1;
    }
    return count;
}

// ---- Rebinding ---------------------------------------------------------------------------------

void ActionMap::rebind(ButtonAction a, int player, int slot, Binding b) {
    Impl &m = *impl_;
    LAM_ASSERT(known(m, a) && valid_player(player) && valid_slot(slot), "ActionMap::rebind: bad action, player or slot");
    if (!known(m, a) || !valid_player(player) || !valid_slot(slot)) return;
    ButtonSlots &s = m.buttons[a.id][static_cast<std::size_t>(player)];
    s.slot[static_cast<std::size_t>(slot)] = b;
    recount(s);
    ++m.revision;
}

void ActionMap::clear_bindings(ButtonAction a, int player) {
    Impl &m = *impl_;
    LAM_ASSERT(known(m, a) && valid_player(player), "ActionMap::clear_bindings: bad action or player");
    if (!known(m, a) || !valid_player(player)) return;
    m.buttons[a.id][static_cast<std::size_t>(player)] = ButtonSlots{};
    ++m.revision;
}

std::span<const Binding> ActionMap::bindings(ButtonAction a, int player) const {
    const Impl &m = *impl_;
    if (!known(m, a) || !valid_player(player)) return {};
    const ButtonSlots &s = m.buttons[a.id][static_cast<std::size_t>(player)];
    return {s.slot.data(), s.count};
}

void ActionMap::rebind(AxisAction a, int player, int slot, const Axis2Binding &b) {
    Impl &m = *impl_;
    LAM_ASSERT(known(m, a) && valid_player(player) && valid_slot(slot), "ActionMap::rebind: bad axis, player or slot");
    if (!known(m, a) || !valid_player(player) || !valid_slot(slot)) return;
    AxisSlots &s = m.axes[a.id][static_cast<std::size_t>(player)];
    s.keys[static_cast<std::size_t>(slot)] = b;
    recount(s);
    ++m.revision;
}

void ActionMap::rebind(AxisAction a, int player, int slot, AxisDir dir, Binding b) {
    Impl &m = *impl_;
    LAM_ASSERT(known(m, a) && valid_player(player) && valid_slot(slot), "ActionMap::rebind: bad axis, player or slot");
    if (!known(m, a) || !valid_player(player) || !valid_slot(slot)) return;
    AxisSlots &s = m.axes[a.id][static_cast<std::size_t>(player)];
    dir_ref(s.keys[static_cast<std::size_t>(slot)], dir) = b;
    recount(s);
    ++m.revision;
}

void ActionMap::bind_stick(AxisAction a, int player, int slot, StickBinding stick) {
    Impl &m = *impl_;
    LAM_ASSERT(known(m, a) && valid_player(player) && valid_slot(slot), "ActionMap::bind_stick: bad axis, player or slot");
    if (!known(m, a) || !valid_player(player) || !valid_slot(slot)) return;
    AxisSlots &s = m.axes[a.id][static_cast<std::size_t>(player)];
    s.sticks[static_cast<std::size_t>(slot)] = stick;
    recount(s);
    ++m.revision;
}

void ActionMap::clear_bindings(AxisAction a, int player) {
    Impl &m = *impl_;
    LAM_ASSERT(known(m, a) && valid_player(player), "ActionMap::clear_bindings: bad axis or player");
    if (!known(m, a) || !valid_player(player)) return;
    AxisSlots &s = m.axes[a.id][static_cast<std::size_t>(player)];
    s = AxisSlots{};
    s.sticks.fill(empty_stick());
    ++m.revision;
}

std::span<const Axis2Binding> ActionMap::axis_bindings(AxisAction a, int player) const {
    const Impl &m = *impl_;
    if (!known(m, a) || !valid_player(player)) return {};
    const AxisSlots &s = m.axes[a.id][static_cast<std::size_t>(player)];
    return {s.keys.data(), s.key_count};
}

std::span<const StickBinding> ActionMap::stick_bindings(AxisAction a, int player) const {
    const Impl &m = *impl_;
    if (!known(m, a) || !valid_player(player)) return {};
    const AxisSlots &s = m.axes[a.id][static_cast<std::size_t>(player)];
    return {s.sticks.data(), s.stick_count};
}

void ActionMap::reset_bindings(int player) {
    Impl &m = *impl_;
    LAM_ASSERT(valid_player(player), "ActionMap::reset_bindings: player out of range");
    if (!valid_player(player)) return;
    const auto p = static_cast<std::size_t>(player);
    for (std::size_t a = 1; a < m.button_names.size(); ++a)
        m.buttons[a][p] = player == 0 ? m.button_defaults[a] : ButtonSlots{};
    for (std::size_t a = 1; a < m.axis_names.size(); ++a) {
        if (player == 0) {
            m.axes[a][p] = m.axis_defaults[a];
        } else {
            m.axes[a][p] = AxisSlots{};
            m.axes[a][p].sticks.fill(empty_stick());
        }
    }
    ++m.revision;
}

// ---- Capture -----------------------------------------------------------------------------------

void ActionMap::begin_capture(ButtonAction a, int player, int slot) {
    Impl &m = *impl_;
    LAM_ASSERT(known(m, a) && valid_player(player) && valid_slot(slot), "ActionMap::begin_capture: bad action, player or slot");
    if (!known(m, a) || !valid_player(player) || !valid_slot(slot)) return;
    m.capture = CaptureKind::Button;
    m.capture_id = a.id;
    m.capture_player = player;
    m.capture_slot = slot;
}

void ActionMap::begin_capture(AxisAction a, int player, int slot, AxisDir dir) {
    Impl &m = *impl_;
    LAM_ASSERT(known(m, a) && valid_player(player) && valid_slot(slot), "ActionMap::begin_capture: bad axis, player or slot");
    if (!known(m, a) || !valid_player(player) || !valid_slot(slot)) return;
    m.capture = CaptureKind::Axis;
    m.capture_id = a.id;
    m.capture_player = player;
    m.capture_slot = slot;
    m.capture_dir = dir;
}

bool ActionMap::capturing() const {
    return impl_->capture != CaptureKind::None;
}

void ActionMap::cancel_capture() {
    impl_->capture = CaptureKind::None;
}

// ---- Touch zones -------------------------------------------------------------------------------

void ActionMap::add_touch_zone(const TouchZone &z) {
    Impl &m = *impl_;
    LAM_ASSERT(m.zone_count < kMaxTouchZones, "ActionMap::add_touch_zone: more than kMaxTouchZones");
    if (m.zone_count >= kMaxTouchZones) return;
    m.zones[static_cast<std::size_t>(m.zone_count++)] = z;
    ++m.revision;
}

std::span<const TouchZone> ActionMap::touch_zones() const {
    return {impl_->zones.data(), static_cast<std::size_t>(impl_->zone_count)};
}

void ActionMap::clear_touch_zones() {
    impl_->zone_count = 0;
    ++impl_->zone_layout; // end held captures even if zones are re-added this frame
    ++impl_->revision;
}

std::uint32_t ActionMap::revision() const {
    return impl_->revision;
}

// ---- Binding factories -------------------------------------------------------------------------

Binding ActionMap::key(Key k) {
    return Binding{BindingSource::Key, static_cast<std::uint16_t>(k), -1, 0.5f};
}

Binding ActionMap::mouse(MouseButton b) {
    return Binding{BindingSource::Mouse, static_cast<std::uint16_t>(b), -1, 0.5f};
}

Binding ActionMap::pad(PadButton b, int pad) {
    LAM_ASSERT(pad >= -1 && pad < kMaxGamepads, "ActionMap::pad: pad must be -1..3");
    return Binding{BindingSource::PadButton, static_cast<std::uint16_t>(b),
                   static_cast<std::int8_t>(std::clamp(pad, -1, kMaxGamepads - 1)), 0.5f};
}

Binding ActionMap::pad_axis(PadAxis a, bool positive, float threshold, int pad) {
    LAM_ASSERT(pad >= -1 && pad < kMaxGamepads, "ActionMap::pad_axis: pad must be -1..3");
    LAM_ASSERT(threshold >= 0.0f && threshold < 1.0f, "ActionMap::pad_axis: threshold must be in [0, 1)");
    return Binding{positive ? BindingSource::PadAxisPositive : BindingSource::PadAxisNegative,
                   static_cast<std::uint16_t>(a), static_cast<std::int8_t>(std::clamp(pad, -1, kMaxGamepads - 1)),
                   std::clamp(threshold, 0.0f, 0.99f)};
}

Binding ActionMap::wheel(bool up) {
    return Binding{up ? BindingSource::WheelUp : BindingSource::WheelDown, 0, -1, 0.5f};
}

Binding ActionMap::touch(int zone) {
    LAM_ASSERT(zone >= 0 && zone < kMaxTouchZones, "ActionMap::touch: zone out of range");
    return Binding{BindingSource::TouchZone, static_cast<std::uint16_t>(std::clamp(zone, 0, kMaxTouchZones - 1)),
                   -1, 0.5f};
}

} // namespace lam
