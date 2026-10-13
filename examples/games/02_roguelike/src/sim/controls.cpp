#include <sim/controls.h>

#include <lam/input/actions.h>
#include <lam/input/keys.h>

namespace rl {
using lam::ActionMap;
using lam::Key;

Controls define_controls(ActionMap &map) {
    Controls c;
    c.up = map.button("move_up",
                      {ActionMap::key(Key::Up), ActionMap::key(Key::W), ActionMap::key(Key::Kp8)});
    c.down = map.button(
        "move_down", {ActionMap::key(Key::Down), ActionMap::key(Key::S), ActionMap::key(Key::Kp2)});
    c.left = map.button(
        "move_left", {ActionMap::key(Key::Left), ActionMap::key(Key::A), ActionMap::key(Key::Kp4)});
    c.right = map.button("move_right", {ActionMap::key(Key::Right), ActionMap::key(Key::D),
                                        ActionMap::key(Key::Kp6)});
    c.wait = map.button("wait", {ActionMap::key(Key::Space), ActionMap::key(Key::Kp5)});
    c.descend = map.button("descend", {ActionMap::key(Key::Period), ActionMap::key(Key::Enter)});
    static constexpr Key kNumbers[8] = {Key::Num1, Key::Num2, Key::Num3, Key::Num4,
                                        Key::Num5, Key::Num6, Key::Num7, Key::Num8};
    static constexpr const char *kNames[8] = {"use_1", "use_2", "use_3", "use_4",
                                              "use_5", "use_6", "use_7", "use_8"};
    for (int i = 0; i < 8; ++i)
        c.use[i] = map.button(kNames[i], {ActionMap::key(kNumbers[i])});
    c.save = map.button("save_game", {ActionMap::key(Key::F5)});
    c.load = map.button("load_game", {ActionMap::key(Key::F9)});
    // Held movement keys repeat: first repeat after 12 ticks, then every 6
    // (InputFrame::is_repeated).
    for (lam::ButtonAction a : {c.up, c.down, c.left, c.right, c.wait})
        map.set_repeat(a, 12, 6);
    return c;
}

} // namespace rl
