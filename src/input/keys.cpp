// Device-code names. Settings files store these, so never rename one; add an alias instead.
// Name lookups scan linearly, which is fine since they only happen at load.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <lam/input/keys.h>

namespace lam {
namespace {

struct KeyEntry {
    Key key;
    std::string_view name;
};

// One canonical name per code.
constexpr KeyEntry kKeyNames[] = {
    {Key::AndroidBack, "android_back"},
    {Key::AndroidMenu, "android_menu"},
    {Key::VolumeUp, "volume_up"},
    {Key::VolumeDown, "volume_down"},
    {Key::Space, "space"},
    {Key::Apostrophe, "apostrophe"},
    {Key::Comma, "comma"},
    {Key::Minus, "minus"},
    {Key::Period, "period"},
    {Key::Slash, "slash"},
    {Key::Num0, "num0"},
    {Key::Num1, "num1"},
    {Key::Num2, "num2"},
    {Key::Num3, "num3"},
    {Key::Num4, "num4"},
    {Key::Num5, "num5"},
    {Key::Num6, "num6"},
    {Key::Num7, "num7"},
    {Key::Num8, "num8"},
    {Key::Num9, "num9"},
    {Key::Semicolon, "semicolon"},
    {Key::Equal, "equal"},
    {Key::A, "a"},
    {Key::B, "b"},
    {Key::C, "c"},
    {Key::D, "d"},
    {Key::E, "e"},
    {Key::F, "f"},
    {Key::G, "g"},
    {Key::H, "h"},
    {Key::I, "i"},
    {Key::J, "j"},
    {Key::K, "k"},
    {Key::L, "l"},
    {Key::M, "m"},
    {Key::N, "n"},
    {Key::O, "o"},
    {Key::P, "p"},
    {Key::Q, "q"},
    {Key::R, "r"},
    {Key::S, "s"},
    {Key::T, "t"},
    {Key::U, "u"},
    {Key::V, "v"},
    {Key::W, "w"},
    {Key::X, "x"},
    {Key::Y, "y"},
    {Key::Z, "z"},
    {Key::LeftBracket, "left_bracket"},
    {Key::Backslash, "backslash"},
    {Key::RightBracket, "right_bracket"},
    {Key::Grave, "grave"},
    {Key::Escape, "escape"},
    {Key::Enter, "enter"},
    {Key::Tab, "tab"},
    {Key::Backspace, "backspace"},
    {Key::Insert, "insert"},
    {Key::Delete, "delete"},
    {Key::Right, "right"},
    {Key::Left, "left"},
    {Key::Down, "down"},
    {Key::Up, "up"},
    {Key::PageUp, "page_up"},
    {Key::PageDown, "page_down"},
    {Key::Home, "home"},
    {Key::End, "end"},
    {Key::CapsLock, "caps_lock"},
    {Key::ScrollLock, "scroll_lock"},
    {Key::NumLock, "num_lock"},
    {Key::PrintScreen, "print_screen"},
    {Key::Pause, "pause"},
    {Key::F1, "f1"},
    {Key::F2, "f2"},
    {Key::F3, "f3"},
    {Key::F4, "f4"},
    {Key::F5, "f5"},
    {Key::F6, "f6"},
    {Key::F7, "f7"},
    {Key::F8, "f8"},
    {Key::F9, "f9"},
    {Key::F10, "f10"},
    {Key::F11, "f11"},
    {Key::F12, "f12"},
    {Key::Kp0, "kp0"},
    {Key::Kp1, "kp1"},
    {Key::Kp2, "kp2"},
    {Key::Kp3, "kp3"},
    {Key::Kp4, "kp4"},
    {Key::Kp5, "kp5"},
    {Key::Kp6, "kp6"},
    {Key::Kp7, "kp7"},
    {Key::Kp8, "kp8"},
    {Key::Kp9, "kp9"},
    {Key::KpDecimal, "kp_decimal"},
    {Key::KpDivide, "kp_divide"},
    {Key::KpMultiply, "kp_multiply"},
    {Key::KpSubtract, "kp_subtract"},
    {Key::KpAdd, "kp_add"},
    {Key::KpEnter, "kp_enter"},
    {Key::KpEqual, "kp_equal"},
    {Key::LShift, "lshift"},
    {Key::LCtrl, "lctrl"},
    {Key::LAlt, "lalt"},
    {Key::LSuper, "lsuper"},
    {Key::RShift, "rshift"},
    {Key::RCtrl, "rctrl"},
    {Key::RAlt, "ralt"},
    {Key::RSuper, "rsuper"},
    {Key::Menu, "menu"},
};

// Extra spellings accepted when parsing only.
constexpr KeyEntry kKeyAliases[] = {
    {Key::Escape, "esc"}, {Key::Enter, "return"}, {Key::Num0, "0"}, {Key::Num1, "1"},
    {Key::Num2, "2"},     {Key::Num3, "3"},       {Key::Num4, "4"}, {Key::Num5, "5"},
    {Key::Num6, "6"},     {Key::Num7, "7"},       {Key::Num8, "8"}, {Key::Num9, "9"},
};

// Code -> name, built at compile time.
constexpr std::array<std::string_view, kKeyCount> kKeyNameByCode = [] {
    std::array<std::string_view, kKeyCount> names{};
    for (const KeyEntry &e : kKeyNames)
        names[static_cast<std::size_t>(e.key)] = e.name;
    return names;
}();

constexpr std::string_view kMouseButtonNames[] = {"left",  "right",   "middle", "side",
                                                  "extra", "forward", "back"};

// Index = PadButton value; Unknown (0) has no name.
constexpr std::string_view kPadButtonNames[kPadButtonCount] = {
    "",   "dpad_up", "dpad_right", "dpad_down", "dpad_left", "north", "east",  "south",  "west",
    "lb", "lt",      "rb",         "rt",        "select",    "guide", "start", "lstick", "rstick"};

constexpr std::string_view kPadAxisNames[kPadAxisCount] = {"left_x",  "left_y", "right_x",
                                                           "right_y", "lt",     "rt"};

static_assert(std::size(kMouseButtonNames) == 7);

constexpr char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// `canonical` is already lower case.
constexpr bool equals_name(std::string_view text, std::string_view canonical) {
    if (text.size() != canonical.size()) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (to_lower_ascii(text[i]) != canonical[i]) return false;
    }
    return true;
}

// Index of `name` in `names` (empty entries never match), or -1.
template <std::size_t N>
int find_name(const std::string_view (&names)[N], std::string_view name) {
    if (name.empty()) return -1;
    for (std::size_t i = 0; i < N; ++i) {
        if (!names[i].empty() && equals_name(name, names[i])) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

std::optional<Key> key_from_name(std::string_view name) {
    if (name.empty()) return std::nullopt;
    for (const KeyEntry &e : kKeyNames) {
        if (equals_name(name, e.name)) return e.key;
    }
    for (const KeyEntry &e : kKeyAliases) {
        if (equals_name(name, e.name)) return e.key;
    }
    return std::nullopt;
}

std::string_view key_name(Key k) {
    const auto code = static_cast<std::size_t>(k);
    return code < kKeyNameByCode.size() ? kKeyNameByCode[code] : std::string_view{};
}

std::optional<MouseButton> mouse_button_from_name(std::string_view name) {
    const int i = find_name(kMouseButtonNames, name);
    if (i < 0) return std::nullopt;
    return static_cast<MouseButton>(i);
}

std::string_view mouse_button_name(MouseButton b) {
    const auto i = static_cast<std::size_t>(b);
    return i < std::size(kMouseButtonNames) ? kMouseButtonNames[i] : std::string_view{};
}

std::optional<PadButton> pad_button_from_name(std::string_view name) {
    const int i = find_name(kPadButtonNames, name);
    if (i < 0) return std::nullopt;
    return static_cast<PadButton>(i);
}

std::string_view pad_button_name(PadButton b) {
    const auto i = static_cast<std::size_t>(b);
    return i < std::size(kPadButtonNames) ? kPadButtonNames[i] : std::string_view{};
}

std::optional<PadAxis> pad_axis_from_name(std::string_view name) {
    const int i = find_name(kPadAxisNames, name);
    if (i < 0) return std::nullopt;
    return static_cast<PadAxis>(i);
}

std::string_view pad_axis_name(PadAxis a) {
    const auto i = static_cast<std::size_t>(a);
    return i < std::size(kPadAxisNames) ? kPadAxisNames[i] : std::string_view{};
}

} // namespace lam
