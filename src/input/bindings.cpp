// Binding strings and ActionMap persistence ("input.p<player>.<action>.<slot>"). Cold path.
#include <lam/input/actions.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

#include <lam/core/log.h>

#include "detail/input_state.h"

namespace lam {
namespace {

constexpr std::string_view kNone = "none";

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Non-negative decimal integer; no sign, no spaces.
std::optional<int> parse_uint(std::string_view s, int max) {
    if (s.empty() || s.size() > 6) return std::nullopt;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
    }
    if (v > max) return std::nullopt;
    return v;
}

// "0.25", ".5", "1". Locale-free.
std::optional<float> parse_fraction(std::string_view s) {
    if (s.empty() || s.size() > 12) return std::nullopt;
    double v = 0, scale = 1;
    bool dot = false, digits = false;
    for (char c : s) {
        if (c == '.' && !dot) {
            dot = true;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        digits = true;
        if (dot) {
            scale /= 10;
            v += (c - '0') * scale;
        } else {
            v = v * 10 + (c - '0');
        }
    }
    if (!digits) return std::nullopt;
    return static_cast<float>(v);
}

std::string format_threshold(float t) {
    std::array<char, 32> buf{};
    auto r = std::to_chars(buf.data(), buf.data() + buf.size(), t, std::chars_format::fixed, 3);
    std::string s(buf.data(), r.ptr);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string pad_suffix(std::int8_t pad) {
    return pad >= 0 ? "@" + std::to_string(static_cast<int>(pad)) : std::string();
}

std::string settings_key(int player, std::string_view action, int slot) {
    std::string k = "input.p";
    k += std::to_string(player);
    k += '.';
    k += action;
    k += '.';
    k += std::to_string(slot);
    return k;
}

// "a,b,c,d" (axis2: up,down,left,right) or "a,b" (axis1: negative,positive).
std::string composite_to_string(const Axis2Binding &b, bool two_d) {
    if (two_d)
        return binding_to_string(b.up) + "," + binding_to_string(b.down) + "," + binding_to_string(b.left) +
               "," + binding_to_string(b.right);
    return binding_to_string(b.left) + "," + binding_to_string(b.right);
}

std::optional<Axis2Binding> composite_from_string(std::string_view s, bool two_d) {
    std::array<std::string_view, 4> parts{};
    std::size_t n = 0;
    while (true) {
        const std::size_t comma = s.find(',');
        if (n >= parts.size()) return std::nullopt;
        parts[n++] = trim(s.substr(0, comma));
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    if (n != (two_d ? 4u : 2u)) return std::nullopt;
    std::array<Binding, 4> b{};
    for (std::size_t i = 0; i < n; ++i) {
        auto one = binding_from_string(parts[i]);
        if (!one) return std::nullopt;
        b[i] = *one;
    }
    Axis2Binding out;
    if (two_d) {
        out.up = b[0];
        out.down = b[1];
        out.left = b[2];
        out.right = b[3];
    } else {
        out.left = b[0];
        out.right = b[1];
    }
    return out;
}

} // namespace

std::string binding_to_string(const Binding &b) {
    switch (b.source) {
    case BindingSource::None: return std::string(kNone);
    case BindingSource::Key: {
        const std::string_view n = key_name(static_cast<Key>(b.code));
        return n.empty() ? "key:#" + std::to_string(b.code) : "key:" + std::string(n);
    }
    case BindingSource::Mouse: {
        const std::string_view n = mouse_button_name(static_cast<MouseButton>(b.code));
        return n.empty() ? std::string(kNone) : "mouse:" + std::string(n);
    }
    case BindingSource::PadButton: {
        const std::string_view n = pad_button_name(static_cast<PadButton>(b.code));
        return n.empty() ? std::string(kNone) : "pad:" + std::string(n) + pad_suffix(b.pad);
    }
    case BindingSource::PadAxisPositive:
    case BindingSource::PadAxisNegative: {
        const std::string_view n = pad_axis_name(static_cast<PadAxis>(b.code));
        if (n.empty()) return std::string(kNone);
        std::string s = "axis:";
        s += b.source == BindingSource::PadAxisPositive ? '+' : '-';
        s += n;
        s += pad_suffix(b.pad);
        if (b.threshold != 0.5f) s += ">" + format_threshold(b.threshold);
        return s;
    }
    case BindingSource::WheelUp: return "wheel:up";
    case BindingSource::WheelDown: return "wheel:down";
    case BindingSource::TouchZone: return "touch:" + std::to_string(b.code);
    }
    return std::string(kNone);
}

std::optional<Binding> binding_from_string(std::string_view s) {
    s = trim(s);
    if (iequals(s, kNone)) return Binding{};
    const std::size_t colon = s.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const std::string_view kind = s.substr(0, colon);
    std::string_view rest = s.substr(colon + 1);
    Binding b;
    if (iequals(kind, "key")) {
        if (!rest.empty() && rest.front() == '#') {
            const auto code = parse_uint(rest.substr(1), kKeyCount - 1);
            if (!code || *code == 0) return std::nullopt;
            return Binding{BindingSource::Key, static_cast<std::uint16_t>(*code), -1, 0.5f};
        }
        const auto k = key_from_name(rest);
        if (!k || *k == Key::None) return std::nullopt;
        return ActionMap::key(*k);
    }
    if (iequals(kind, "mouse")) {
        const auto m = mouse_button_from_name(rest);
        if (!m) return std::nullopt;
        return ActionMap::mouse(*m);
    }
    if (iequals(kind, "wheel")) {
        if (iequals(rest, "up")) return ActionMap::wheel(true);
        if (iequals(rest, "down")) return ActionMap::wheel(false);
        return std::nullopt;
    }
    if (iequals(kind, "touch")) {
        const auto z = parse_uint(rest, kMaxTouchZones - 1);
        if (!z) return std::nullopt;
        return ActionMap::touch(*z);
    }
    // pad and axis: optional ">threshold" (axis only), then optional "@pad".
    float threshold = 0.5f;
    const bool axis = iequals(kind, "axis");
    if (!axis && !iequals(kind, "pad")) return std::nullopt;
    if (const std::size_t gt = rest.find('>'); gt != std::string_view::npos) {
        if (!axis) return std::nullopt;
        const auto t = parse_fraction(rest.substr(gt + 1));
        if (!t || *t >= 1.0f) return std::nullopt;
        threshold = *t;
        rest = rest.substr(0, gt);
    }
    int pad = -1;
    if (const std::size_t at = rest.find('@'); at != std::string_view::npos) {
        const auto p = parse_uint(rest.substr(at + 1), kMaxGamepads - 1);
        if (!p) return std::nullopt;
        pad = *p;
        rest = rest.substr(0, at);
    }
    if (!axis) {
        const auto pb = pad_button_from_name(rest);
        if (!pb || *pb == PadButton::Unknown) return std::nullopt;
        return ActionMap::pad(*pb, pad);
    }
    if (rest.empty() || (rest.front() != '+' && rest.front() != '-')) return std::nullopt;
    const bool positive = rest.front() == '+';
    const auto pa = pad_axis_from_name(rest.substr(1));
    if (!pa) return std::nullopt;
    b = ActionMap::pad_axis(*pa, positive, threshold, pad);
    return b;
}

// ---- Persistence -------------------------------------------------------------------------------

// The first time a pair is seen, remember its bindings as the defaults. Players 1..3 can only be
// bound with rebind(), so whatever is in memory now has to count as their default. The Impl
// tables are bookkeeping, hence the writes through a const map.
void ActionMap::define_settings(SettingsStore &s) const {
    Impl &m = *impl_;
    const int players = player_count();
    for (int p = 0; p < players; ++p) {
        const auto pi = static_cast<std::size_t>(p);
        for (std::size_t a = 1; a < m.button_names.size(); ++a) {
            const bool take = !m.button_def_taken[a][pi];
            m.button_def_taken[a][pi] = true;
            for (int slot = 0; slot < kMaxBindingSlots; ++slot) {
                const auto si = static_cast<std::size_t>(slot);
                const std::string key = settings_key(p, m.button_names[a], slot);
                const bool defined = s.defined(key);
                if (take) {
                    m.button_def[a][pi][si] = !defined ? m.buttons[a][pi].slot[si]
                                              : p == 0 ? m.button_defaults[a].slot[si]
                                                       : Binding{};
                }
                if (!defined) s.define_string(key, binding_to_string(m.button_def[a][pi][si]));
            }
        }
        for (std::size_t a = 1; a < m.axis_names.size(); ++a) {
            const bool take = !m.axis_def_taken[a][pi];
            m.axis_def_taken[a][pi] = true;
            for (int slot = 0; slot < kMaxBindingSlots; ++slot) {
                const auto si = static_cast<std::size_t>(slot);
                const std::string key = settings_key(p, m.axis_names[a], slot);
                const bool defined = s.defined(key);
                if (take) {
                    m.axis_def[a][pi][si] = !defined ? m.axes[a][pi].keys[si]
                                            : p == 0 ? m.axis_defaults[a].keys[si]
                                                     : Axis2Binding{};
                }
                if (!defined) s.define_string(key, composite_to_string(m.axis_def[a][pi][si], m.axis_2d[a]));
            }
        }
    }
}

Status ActionMap::save(SettingsStore &s) const {
    const Impl &m = *impl_;
    define_settings(s);
    const int players = player_count();
    for (int p = 0; p < players; ++p) {
        const auto pi = static_cast<std::size_t>(p);
        for (std::size_t a = 1; a < m.button_names.size(); ++a)
            for (int slot = 0; slot < kMaxBindingSlots; ++slot)
                s.set_string(settings_key(p, m.button_names[a], slot),
                             binding_to_string(m.buttons[a][pi].slot[static_cast<std::size_t>(slot)]));
        for (std::size_t a = 1; a < m.axis_names.size(); ++a)
            for (int slot = 0; slot < kMaxBindingSlots; ++slot)
                s.set_string(settings_key(p, m.axis_names[a], slot),
                             composite_to_string(m.axes[a][pi].keys[static_cast<std::size_t>(slot)], m.axis_2d[a]));
    }
    return {};
}

Status ActionMap::load(const SettingsStore &s) {
    Impl &m = *impl_;
    bool changed = false;
    for (int p = 0; p < kMaxPlayers; ++p) {
        const auto pi = static_cast<std::size_t>(p);
        for (std::size_t a = 1; a < m.button_names.size(); ++a) {
            ButtonSlots &slots = m.buttons[a][pi];
            for (int slot = 0; slot < kMaxBindingSlots; ++slot) {
                const std::string key = settings_key(p, m.button_names[a], slot);
                if (!s.defined(key)) continue;
                m.button_synced[a][pi] = true;
                const std::string &value = s.get_string(key);
                const auto b = binding_from_string(value);
                if (!b) {
                    LAM_LOG_WARN("input: settings \"{}\" = \"{}\" is not a binding; keeping the current one", key,
                                 value);
                    continue;
                }
                Binding &dst = slots.slot[static_cast<std::size_t>(slot)];
                if (dst == *b) continue;
                dst = *b;
                changed = true;
            }
            std::uint8_t n = 0;
            for (int i = 0; i < kMaxBindingSlots; ++i)
                if (slots.slot[static_cast<std::size_t>(i)].source != BindingSource::None)
                    n = static_cast<std::uint8_t>(i + 1);
            slots.count = n;
        }
        for (std::size_t a = 1; a < m.axis_names.size(); ++a) {
            AxisSlots &slots = m.axes[a][pi];
            for (int slot = 0; slot < kMaxBindingSlots; ++slot) {
                const std::string key = settings_key(p, m.axis_names[a], slot);
                if (!s.defined(key)) continue;
                m.axis_synced[a][pi] = true;
                const std::string &value = s.get_string(key);
                const auto b = composite_from_string(value, m.axis_2d[a]);
                if (!b) {
                    LAM_LOG_WARN("input: settings \"{}\" = \"{}\" is not an axis binding; keeping the current one",
                                 key, value);
                    continue;
                }
                Axis2Binding &dst = slots.keys[static_cast<std::size_t>(slot)];
                if (dst == *b) continue;
                dst = *b;
                changed = true;
            }
            std::uint8_t n = 0;
            for (int i = 0; i < kMaxBindingSlots; ++i) {
                const Axis2Binding &k = slots.keys[static_cast<std::size_t>(i)];
                if (k.up.source != BindingSource::None || k.down.source != BindingSource::None ||
                    k.left.source != BindingSource::None || k.right.source != BindingSource::None)
                    n = static_cast<std::uint8_t>(i + 1);
            }
            slots.key_count = n;
        }
    }
    if (changed) ++m.revision;
    return {};
}

namespace detail {

void sync_settings(ActionMap &map, SettingsStore &s) {
    InputAccess::MapImpl &m = InputAccess::map(map);
    map.define_settings(s);
    const int players = map.player_count();
    bool changed = false;
    for (int p = 0; p < players; ++p) {
        const auto pi = static_cast<std::size_t>(p);
        for (std::size_t a = 1; a < m.button_names.size(); ++a) {
            if (m.button_synced[a][pi]) continue;
            m.button_synced[a][pi] = true;
            ButtonSlots &slots = m.buttons[a][pi];
            for (int slot = 0; slot < kMaxBindingSlots; ++slot) {
                const auto si = static_cast<std::size_t>(slot);
                const std::string &value = s.get_string(settings_key(p, m.button_names[a], slot));
                const Binding &def = m.button_def[a][pi][si];
                if (slots.slot[si] != def || value == binding_to_string(def)) continue;   // rebound in memory, or not from the file
                const auto b = binding_from_string(value);
                if (!b || slots.slot[si] == *b) continue;
                slots.slot[si] = *b;
                changed = true;
            }
            recount(slots);
        }
        for (std::size_t a = 1; a < m.axis_names.size(); ++a) {
            if (m.axis_synced[a][pi]) continue;
            m.axis_synced[a][pi] = true;
            AxisSlots &slots = m.axes[a][pi];
            for (int slot = 0; slot < kMaxBindingSlots; ++slot) {
                const auto si = static_cast<std::size_t>(slot);
                const std::string &value = s.get_string(settings_key(p, m.axis_names[a], slot));
                const Axis2Binding &def = m.axis_def[a][pi][si];
                if (slots.keys[si] != def || value == composite_to_string(def, m.axis_2d[a])) continue;
                const auto b = composite_from_string(value, m.axis_2d[a]);
                if (!b || slots.keys[si] == *b) continue;
                slots.keys[si] = *b;
                changed = true;
            }
            recount(slots);
        }
    }
    if (changed) ++m.revision;
    (void)map.save(s);
}

} // namespace detail

} // namespace lam
