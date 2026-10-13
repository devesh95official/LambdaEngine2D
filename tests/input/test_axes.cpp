// Axes, deadzones, gamepads and per-player pad assignment.
#include "input_test_util.h"

#include <cmath>

using namespace lam;
using lam::test::StepApp;

namespace {

Vec2 frame_axis(StepApp &t, AxisAction a) {
    return t.frame_input().axis2(a);
}

} // namespace

TEST_CASE("axis2: WASD composite; diagonals have length 1; opposite keys cancel") {
    StepApp t;
    const AxisAction move = t.map().axis2(
        "move", {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A), ActionMap::key(Key::D)}});
    CHECK(t.map().is_axis2(move));
    t.frame(1, test::keys_down({Key::D}));
    CHECK(frame_axis(t, move) == Vec2{1, 0});
    t.frame(1, test::keys_down({Key::W}));
    CHECK(frame_axis(t, move) == Vec2{0, -1});                  // +y is down
    t.frame(1, test::keys_down({Key::S, Key::D}));
    const Vec2 diag = frame_axis(t, move);
    CHECK(length(diag) == doctest::Approx(1.0f));
    CHECK(diag.x == doctest::Approx(std::sqrt(0.5f)));
    CHECK(diag.y == doctest::Approx(std::sqrt(0.5f)));
    t.frame(1, test::keys_down({Key::A, Key::D}));
    CHECK(frame_axis(t, move) == Vec2{0, 0});
    // The tick frame carries the same value (shared by every tick of the frame).
    t.frame(2, test::keys_down({Key::A}));
    const auto &frames = t.probe().frames;
    REQUIRE(frames.size() >= 2);
    CHECK(frames[frames.size() - 1].axis2(move) == Vec2{-1, 0});
    CHECK(frames[frames.size() - 2].axis2(move) == Vec2{-1, 0});
}

TEST_CASE("axis2: stick radial deadzone rescales; composites and sticks add, clamped to 1") {
    StepApp t;
    const AxisAction move = t.map().axis2(
        "move", {{ActionMap::key(Key::Up), ActionMap::key(Key::Down), ActionMap::key(Key::Left), ActionMap::key(Key::Right)}},
        {{PadAxis::LeftX, PadAxis::LeftY, 0.2f}});
    t.frame(1, test::with_pad({}, 0, {}, {{PadAxis::LeftX, 0.1f}, {PadAxis::LeftY, 0.1f}}));
    CHECK(frame_axis(t, move) == Vec2{0, 0});                   // inside the deadzone
    t.frame(1, test::with_pad({}, 0, {}, {{PadAxis::LeftX, 0.6f}}));
    CHECK(frame_axis(t, move).x == doctest::Approx(0.5f));      // (0.6 - 0.2) / 0.8
    t.frame(1, test::with_pad({}, 0, {}, {{PadAxis::LeftX, -1.0f}}));
    CHECK(frame_axis(t, move).x == doctest::Approx(-1.0f));
    t.frame(1, test::with_pad(test::keys_down({Key::Right}), 0, {}, {{PadAxis::LeftX, 1.0f}}));
    CHECK(frame_axis(t, move).x == doctest::Approx(1.0f));      // clamped
    t.frame(1, test::with_pad(test::keys_down({Key::Up}), 0, {}, {{PadAxis::LeftX, 1.0f}}));
    CHECK(length(frame_axis(t, move)) == doctest::Approx(1.0f));
    // A disconnected pad reads nothing.
    RawInput r = test::with_pad({}, 0, {}, {{PadAxis::LeftX, 1.0f}});
    r.pads[0].connected = false;
    t.frame(1, r);
    CHECK(frame_axis(t, move) == Vec2{0, 0});
}

TEST_CASE("axis1: positive minus negative; pad axes are analog beyond their threshold") {
    StepApp t;
    const AxisAction throttle = t.map().axis1("throttle", {ActionMap::key(Key::W), ActionMap::pad_axis(PadAxis::RT, true, 0.0f)},
                                              {ActionMap::key(Key::S), ActionMap::pad_axis(PadAxis::LT, true, 0.0f)});
    CHECK_FALSE(t.map().is_axis2(throttle));
    t.frame(1, test::keys_down({Key::W}));
    CHECK(t.frame_input().axis2(throttle) == Vec2{1, 0});
    t.frame(1, test::with_pad({}, 0, {}, {{PadAxis::RT, 0.25f}}));
    CHECK(t.frame_input().axis2(throttle).x == doctest::Approx(0.25f));
    t.frame(1, test::with_pad(test::keys_down({Key::W}), 0, {}, {{PadAxis::LT, 0.5f}}));
    CHECK(t.frame_input().axis2(throttle).x == doctest::Approx(0.5f));
    t.frame(1, test::keys_down({Key::W, Key::S}));
    CHECK(t.frame_input().axis2(throttle).x == doctest::Approx(0.0f));
    const auto &frames = t.probe().frames;
    REQUIRE(!frames.empty());
    CHECK(frames.back().axis1(throttle) == doctest::Approx(0.0f));
}

TEST_CASE("pads: right stick, triggers as buttons and pad-axis bindings with thresholds") {
    StepApp t;
    const AxisAction aim = t.map().axis2("aim", {}, {{PadAxis::RightX, PadAxis::RightY, 0.25f}});
    const ButtonAction shoot = t.map().button("shoot", {ActionMap::pad_axis(PadAxis::RT, true, 0.5f)});
    const ButtonAction left = t.map().button("left", {ActionMap::pad_axis(PadAxis::LeftX, false, 0.5f)});
    t.frame(1, test::with_pad({}, 0, {}, {{PadAxis::RightX, 0.0f}, {PadAxis::RightY, 1.0f}, {PadAxis::RT, 0.4f}}));
    CHECK(t.frame_input().axis2(aim) == Vec2{0, 1});
    CHECK_FALSE(t.frame_input().is_held(shoot));
    t.frame(1, test::with_pad({}, 0, {}, {{PadAxis::RT, 0.6f}, {PadAxis::LeftX, -0.7f}}));
    CHECK(t.frame_input().is_pressed(shoot));
    CHECK(t.frame_input().is_pressed(left));
    t.frame(1, test::with_pad({}, 0, {}, {{PadAxis::LeftX, 0.7f}}));
    CHECK(t.frame_input().is_released(left));
}

TEST_CASE("multi-pad: assigned pads drive their own player; explicit pads ignore assignment") {
    StepApp t;
    ActionMap &map = t.map();
    const ButtonAction jump = map.button("jump", {ActionMap::pad(PadButton::South)});
    const AxisAction move = map.axis2("move", {}, {{PadAxis::LeftX, PadAxis::LeftY}});
    const ButtonAction special = map.button("special", {ActionMap::pad(PadButton::North, 2)});
    map.rebind(jump, 1, 0, ActionMap::pad(PadButton::South));
    map.bind_stick(move, 1, 0, StickBinding{});
    CHECK(map.stick_bindings(move, 1).size() == 1);
    map.assign_player(0, 0, true);
    map.assign_player(1, 1, false);
    CHECK(map.player_pad(1) == 1);
    CHECK_FALSE(map.player_keyboard_mouse(1));
    CHECK(map.player_count() == 2);

    RawInput r = test::with_pad({}, 0, {}, {});
    r = test::with_pad(r, 1, {PadButton::South}, {{PadAxis::LeftX, 1.0f}});
    t.frame(1, r);
    const auto &p0 = t.probe().frames;
    const auto &p1 = t.probe().p1;
    REQUIRE(p0.size() == 1);
    CHECK_FALSE(p0[0].is_pressed(jump));
    CHECK(p0[0].axis2(move) == Vec2{0, 0});
    CHECK(p1[0].is_pressed(jump));
    CHECK(p1[0].axis2(move).x == doctest::Approx(1.0f));

    // Player 0's explicit pad-2 binding reads pad 2 although player 0 is assigned pad 0.
    t.frame(1, test::with_pad({}, 2, {PadButton::North}));
    CHECK(p0.back().is_pressed(special));

    // kAnyPad (the default for player 0) reads every connected pad.
    map.assign_player(0, kAnyPad, true);
    t.frame(1, test::with_pad({}, 3, {PadButton::South}));
    CHECK(p0.back().is_pressed(jump));
    // kNoPad: player 0 reads no pad.
    map.assign_player(0, kNoPad, true);
    t.frame(1, RawInput{});
    t.frame(1, test::with_pad({}, 3, {PadButton::South}));
    CHECK_FALSE(p0.back().is_pressed(jump));
}

TEST_CASE("keyboard_mouse = false gates key, mouse and wheel bindings of that player") {
    StepApp t;
    ActionMap &map = t.map();
    const ButtonAction use = map.button("use", {ActionMap::key(Key::E), ActionMap::mouse(MouseButton::Right),
                                               ActionMap::wheel(true)});
    map.assign_player(0, kAnyPad, false);
    RawInput r = test::keys_down({Key::E});
    r.mouse_down = 2;
    r.wheel = 1;
    t.frame(1, r);
    CHECK_FALSE(t.frame_input().is_held(use));
    map.assign_player(0, kAnyPad, true);
    t.frame(1, RawInput{});
    RawInput w;
    w.wheel = 1;
    t.frame(1, w);
    CHECK(t.frame_input().is_pressed(use));                     // wheel up
    t.frame(1);                                                 // no feed: the wheel delta does not repeat
    CHECK_FALSE(t.frame_input().is_held(use));
}
