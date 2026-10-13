#pragma once
// Device codes, equal to the raylib/GLFW values so the backend can cast them.
// They're stored in settings and replays, so don't renumber.
#include <cstdint>
#include <optional>
#include <string_view>
namespace lam {
enum class Key : std::uint16_t {
    None = 0, AndroidBack = 4, AndroidMenu = 5, VolumeUp = 24, VolumeDown = 25,
    Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    Num0 = 48, Num1 = 49, Num2 = 50, Num3 = 51, Num4 = 52, Num5 = 53, Num6 = 54, Num7 = 55, Num8 = 56,
    Num9 = 57, Semicolon = 59, Equal = 61,
    A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71, H = 72, I = 73, J = 74, K = 75, L = 76, M = 77,
    N = 78, O = 79, P = 80, Q = 81, R = 82, S = 83, T = 84, U = 85, V = 86, W = 87, X = 88, Y = 89, Z = 90,
    LeftBracket = 91, Backslash = 92, RightBracket = 93, Grave = 96,
    Escape = 256, Enter = 257, Tab = 258, Backspace = 259, Insert = 260, Delete = 261,
    Right = 262, Left = 263, Down = 264, Up = 265, PageUp = 266, PageDown = 267, Home = 268, End = 269,
    CapsLock = 280, ScrollLock = 281, NumLock = 282, PrintScreen = 283, Pause = 284,
    F1 = 290, F2 = 291, F3 = 292, F4 = 293, F5 = 294, F6 = 295, F7 = 296, F8 = 297, F9 = 298, F10 = 299,
    F11 = 300, F12 = 301,
    Kp0 = 320, Kp1 = 321, Kp2 = 322, Kp3 = 323, Kp4 = 324, Kp5 = 325, Kp6 = 326, Kp7 = 327, Kp8 = 328,
    Kp9 = 329, KpDecimal = 330, KpDivide = 331, KpMultiply = 332, KpSubtract = 333, KpAdd = 334,
    KpEnter = 335, KpEqual = 336,
    LShift = 340, LCtrl = 341, LAlt = 342, LSuper = 343, RShift = 344, RCtrl = 345, RAlt = 346,
    RSuper = 347, Menu = 348,
};
inline constexpr int kKeyCount = 349;                           // RawInput::keys_down holds 384
enum class MouseButton : std::uint8_t { Left = 0, Right = 1, Middle = 2, Side = 3, Extra = 4, Forward = 5, Back = 6 };
enum class PadButton : std::uint8_t { Unknown = 0, DUp = 1, DRight = 2, DDown = 3, DLeft = 4, North = 5,
    East = 6, South = 7, West = 8, LB = 9, LT = 10, RB = 11, RT = 12, Select = 13, Guide = 14, Start = 15,
    LStick = 16, RStick = 17 };
inline constexpr int kPadButtonCount = 18;
enum class PadAxis : std::uint8_t { LeftX = 0, LeftY = 1, RightX = 2, RightY = 3, LT = 4, RT = 5 };
inline constexpr int kPadAxisCount = 6;
// Lower-case snake names: "space", "a", "num1", "f11", "left", "lshift", "kp_enter".
std::optional<Key> key_from_name(std::string_view name);
std::string_view key_name(Key k);                               // "" for codes with no name
std::optional<MouseButton> mouse_button_from_name(std::string_view name);
std::string_view mouse_button_name(MouseButton b);
std::optional<PadButton> pad_button_from_name(std::string_view name);
std::string_view pad_button_name(PadButton b);
std::optional<PadAxis> pad_axis_from_name(std::string_view name);
std::string_view pad_axis_name(PadAxis a);
}
