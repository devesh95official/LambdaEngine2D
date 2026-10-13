// Pointer, touch zones, wheel and text input.
#include "input_test_util.h"

#include <lam/render/camera.h>
#include <lam/render/virtual_screen.h>

using namespace lam;
using lam::test::StepApp;

namespace {

RawInput touches(std::initializer_list<RawTouch> list) {
    RawInput r;
    for (const RawTouch &t : list)
        r.touches[static_cast<std::size_t>(r.touch_count++)] = t;
    return r;
}

} // namespace

TEST_CASE("pointer: framebuffer -> virtual through VirtualLayout -> world through the focused CameraView") {
    StepApp t;
    // 960x540 framebuffer, 480x270 virtual, pixel perfect: scale 2, no letterbox.
    t.app().insert_resource<VirtualLayout>(compute_virtual_layout({960, 540}, {480, 270}, ScaleMode::PixelPerfect));
    Camera2D cam;
    cam.position = {1000, 500};
    cam.zoom = 2;
    t.app().main_world().resources().insert<CameraView>(make_camera_view(cam, {480, 270}, {}, false));

    RawInput r;
    r.mouse_fb = {480, 270};                                    // screen centre
    r.mouse_valid = true;
    r.mouse_down = 1;
    t.frame(1, r);
    const PointerState &p = t.frame_input().pointer;
    CHECK(p.valid);
    CHECK(p.virtual_pos == Vec2{240, 135});
    CHECK(p.world.x == doctest::Approx(1000.0f));
    CHECK(p.world.y == doctest::Approx(500.0f));
    CHECK(p.pressed == 1);
    CHECK(p.down == 1);
    const InputFrame &f = t.probe().frames.back();
    CHECK(f.pointer_valid);
    CHECK(f.pointer_virtual == Vec2{240, 135});
    CHECK(f.pointer_world.x == doctest::Approx(1000.0f));
    CHECK(f.pointer_pressed == 1);

    r.mouse_fb = {960 + 10, 0};                                 // outside the viewport
    t.frame(0, r);
    CHECK_FALSE(t.frame_input().pointer.valid);
    CHECK(t.frame_input().pointer.pressed == 0);                // held, not new
    r.mouse_down = 0;
    t.frame(1, r);
    CHECK(t.frame_input().pointer.released == 1);
    CHECK(t.probe().frames.back().pointer_pressed == 0);        // delivered once
}

TEST_CASE("pointer: identity without a layout or camera; presses latch across 0-tick frames") {
    StepApp t;
    RawInput r;
    r.mouse_fb = {12, 34};
    r.mouse_valid = true;
    r.mouse_down = 2;                                           // right
    t.frame(0, r);
    r.mouse_down = 0;
    t.frame(0, r);
    t.frame(2, r);
    const auto &frames = t.probe().frames;
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].pointer_pressed == 2);
    CHECK(frames[0].pointer_down == 0);
    CHECK(frames[1].pointer_pressed == 0);
    CHECK(frames[0].pointer_world == Vec2{12, 34});
    CHECK(frames[0].pointer_virtual == Vec2{12, 34});
}

TEST_CASE("wheel: each notch reaches one tick; FrameInput sees this frame's motion") {
    StepApp t;
    RawInput r;
    r.wheel = 1;
    t.frame(0, r);
    CHECK(t.frame_input().pointer.wheel == 1);
    r.wheel = 2;
    t.frame(0, r);
    t.frame(3);                                                 // stale raw: no new motion
    CHECK(t.frame_input().pointer.wheel == 0);
    const auto &frames = t.probe().frames;
    REQUIRE(frames.size() == 3);
    CHECK(frames[0].wheel == 3);
    CHECK(frames[1].wheel == 0);
}

TEST_CASE("touch: touch 0 is the pointer and counts as the left mouse button") {
    StepApp t;
    const ButtonAction tap = t.map().button("tap", {ActionMap::mouse(MouseButton::Left)});
    t.frame(1, touches({RawTouch{7, {100, 50}, true}}));
    CHECK(t.frame_input().pointer.valid);
    CHECK(t.frame_input().pointer.virtual_pos == Vec2{100, 50});
    CHECK(t.frame_input().is_pressed(tap));
    CHECK(t.probe().frames.back().is_pressed(tap));
    t.frame(1, RawInput{});
    CHECK(t.frame_input().is_released(tap));
}

TEST_CASE("touch zones: a touch that begins inside captures the zone; floating stick; multi-touch") {
    StepApp t;
    ActionMap &map = t.map();
    const ButtonAction jump = map.button("jump");
    const AxisAction move = map.axis2("move", {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A),
                                                ActionMap::key(Key::D)}});
    const ButtonAction fire = map.button("fire", {ActionMap::touch(2)});
    map.add_touch_zone({Rect{0, 0, 200, 200}, ButtonAction{}, move});        // zone 0: stick
    map.add_touch_zone({Rect{400, 100, 80, 80}, jump, AxisAction{}});        // zone 1: button
    map.add_touch_zone({Rect{300, 0, 50, 50}, ButtonAction{}, AxisAction{}});// zone 2: via binding
    CHECK(map.touch_zones().size() == 3);

    // Stick: began at (100,100); radius = 100.
    t.frame(1, touches({RawTouch{1, {100, 100}, true}}));
    CHECK(t.frame_input().axis2(move) == Vec2{0, 0});
    t.frame(1, touches({RawTouch{1, {150, 100}, false}}));
    CHECK(t.frame_input().axis2(move).x == doctest::Approx(0.5f));
    t.frame(1, touches({RawTouch{1, {100, 400}, false}}));      // dragged outside: still captured
    CHECK(t.frame_input().axis2(move).y == doctest::Approx(1.0f));
    // Second finger on the jump zone while the stick is held.
    t.frame(1, touches({RawTouch{1, {100, 400}, false}, RawTouch{2, {410, 110}, true}}));
    CHECK(t.frame_input().is_pressed(jump));
    CHECK(t.probe().frames.back().is_pressed(jump));
    CHECK(t.frame_input().axis2(move).y == doctest::Approx(1.0f));
    // A touch that begins outside every zone, then slides in, captures nothing.
    t.frame(1, touches({RawTouch{3, {600, 300}, true}}));
    CHECK(t.frame_input().is_released(jump));
    CHECK(t.frame_input().axis2(move) == Vec2{0, 0});
    t.frame(1, touches({RawTouch{3, {410, 110}, false}}));
    CHECK_FALSE(t.frame_input().is_held(jump));
    // The binding form (BindingSource::TouchZone), detected as new without the began flag.
    t.frame(1, touches({RawTouch{4, {310, 10}, false}}));
    CHECK(t.frame_input().is_pressed(fire));
    t.frame(1, RawInput{});
    CHECK(t.frame_input().is_released(fire));
    map.clear_touch_zones();
    t.frame(1, touches({RawTouch{5, {310, 10}, true}}));
    CHECK_FALSE(t.frame_input().is_held(fire));
}

TEST_CASE("text input: characters typed this frame; stale frames type nothing") {
    StepApp t;
    RawInput r;
    const char32_t typed[] = {U'h', U'é', U'✓'};
    for (char32_t c : typed)
        r.text[static_cast<std::size_t>(r.text_count++)] = c;
    t.frame(0, r);
    auto text = t.input().text_typed();
    REQUIRE(text.size() == 3);
    CHECK(text[1] == U'é');
    CHECK(text[2] == U'✓');
    t.frame(0);
    CHECK(t.input().text_typed().empty());
    r.text_count = 99;                                          // clamped
    t.frame(0, r);
    CHECK(t.input().text_typed().size() == static_cast<std::size_t>(kMaxTextPerFrame));
}
