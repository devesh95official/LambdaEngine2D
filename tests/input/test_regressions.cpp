// lam_input regressions. Each one failed before its fix.
#include "input_test_util.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <lam/core/fs.h>
#include <lam/core/settings.h>

using namespace lam;
using lam::test::StepApp;

namespace {

RawInput touch_at(int id, Vec2 pos, bool began) {
    RawInput r;
    r.touches[0] = RawTouch{id, pos, began};
    r.touch_count = 1;
    return r;
}

std::unique_ptr<App> headless_input_app() {
    static const char *const argv[] = {"lam_input_tests", "--headless"};
    auto app = std::make_unique<App>(test::StepApp::make_config(), 2, argv);
    app->add_plugin<InputPlugin>();
    return app;
}

} // namespace

TEST_CASE("bindings set in code before their settings keys exist are kept and saved") {
    SUBCASE("fresh install: rebinds made before run() survive finish") {
        auto app = headless_input_app();
        ActionMap &map = app->res<ActionMap>();
        const ButtonAction jump = map.button("jump", {ActionMap::key(Key::W)});
        const ButtonAction fire = map.button("fire", {ActionMap::key(Key::F)});
        map.rebind(jump, 1, 0, ActionMap::key(Key::Up));        // split-keyboard player 2
        map.rebind(fire, 0, 0, ActionMap::key(Key::G));
        app->run_frames(2);
        REQUIRE(map.bindings(jump, 1).size() == 1);
        CHECK(map.bindings(jump, 1)[0] == ActionMap::key(Key::Up)); // was cleared by "none"
        CHECK(map.bindings(fire)[0] == ActionMap::key(Key::G));     // was reverted to key:f
        const SettingsStore &s = app->res<SettingsStore>();
        CHECK(s.get_string("input.p1.jump.0") == "key:up");
        CHECK(s.get_string("input.p0.fire.0") == "key:g");
    }
    SUBCASE("a saved rebind of player 2 wins over the binding code gives after finish") {
        test::TempDir dir;
        const std::string path = dir.file("settings.json");
        REQUIRE(write_file_atomic(path, R"({"input.p1.jump.0": "key:rctrl"})"));
        auto app = headless_input_app();
        REQUIRE(app->res<SettingsStore>().load(path));         // as the App does before plugins build
        ActionMap &map = app->res<ActionMap>();
        const ButtonAction jump = map.button("jump", {ActionMap::key(Key::W)});
        app->run_frames(1);                                     // finish: one player counted
        map.rebind(jump, 1, 0, ActionMap::key(Key::Up));        // e.g. a startup system
        app->run_frames(2);
        REQUIRE(map.bindings(jump, 1).size() == 1);
        CHECK(map.bindings(jump, 1)[0] == ActionMap::key(Key::RCtrl)); // the user's rebind
        CHECK(app->res<SettingsStore>().get_string("input.p1.jump.0") == "key:rctrl"); // not overwritten
    }
}

TEST_CASE("replay records that are not valid InputFrames are refused") {
    test::TempDir dir;
    const std::string path = dir.file("run.lamr");
    {
        InputRecorder rec;
        REQUIRE(rec.open(path, current_replay_header(1, 64, 1)));
        std::array<InputFrame, kMaxPlayers> frames{};
        for (std::uint64_t tick = 1; tick <= 3; ++tick)
            rec.record(tick, frames);
        REQUIRE(rec.close());
    }
    auto bytes = read_file_bytes(path);
    REQUIRE(bytes);
    REQUIRE(ReplayDriver::open(path));                          // the clean file opens
    const std::size_t body = bytes->size() - 3 * (8 + sizeof(InputFrame));
    const auto patched = [&](std::size_t field, std::byte value) {
        std::vector<std::byte> b = *bytes;
        b[body + 8 + field] = value;                            // record 0, player 0
        const std::string p = dir.file("bad.lamr");
        REQUIRE(write_file_atomic(p, std::string_view(reinterpret_cast<const char *>(b.data()), b.size())));
        return ReplayDriver::open(p);
    };
    auto many_intents = patched(offsetof(InputFrame, intent_count), std::byte{200});
    REQUIRE_FALSE(many_intents);
    CHECK(many_intents.error().code == ErrorCode::Parse);
    auto bad_bool = patched(offsetof(InputFrame, pointer_valid), std::byte{7});
    REQUIRE_FALSE(bad_bool);
    CHECK(bad_bool.error().code == ErrorCode::Parse);
    CHECK(patched(offsetof(InputFrame, intent_count), std::byte{kMaxIntents}));   // the limit itself is fine
}

TEST_CASE("name_of views stay valid while more actions are defined") {
    ActionMap map;
    const ButtonAction jump = map.button("jump");
    const AxisAction move = map.axis2("move");
    const std::string_view jump_name = map.name_of(jump);
    const std::string_view move_name = map.name_of(move);
    for (int i = 0; i < 40; ++i)
        (void)map.button("button_" + std::to_string(i));
    for (int i = 0; i < 10; ++i)
        (void)map.axis1("axis_" + std::to_string(i), {}, {});
    CHECK(jump_name.data() == map.name_of(jump).data());       // the same storage, never moved
    CHECK(move_name.data() == map.name_of(move).data());
    CHECK(jump_name == "jump");
    CHECK(move_name == "move");
}

TEST_CASE("a capture swallows the pointer press, wheel, text and starts repeat over") {
    StepApp t;
    ActionMap &map = t.map();
    const ButtonAction jump = map.button("jump", {ActionMap::key(Key::Space)});
    const ButtonAction down = map.button("ui.down", {ActionMap::key(Key::Down)});
    map.set_repeat(down, 18, 4);
    t.frame(1, RawInput{});

    // A click binds the mouse button; neither FrameInput nor the tick sees a pointer press.
    map.begin_capture(jump, 0, 1);
    RawInput click;
    click.mouse_valid = true;
    click.mouse_fb = {10, 10};
    click.mouse_down = 1u << static_cast<unsigned>(MouseButton::Left);
    t.frame(1, click);
    CHECK_FALSE(map.capturing());
    CHECK(map.bindings(jump)[1] == ActionMap::mouse(MouseButton::Left));
    CHECK(t.frame_input().pointer.pressed == 0);
    CHECK(t.probe().frames.back().pointer_pressed == 0);
    RawInput up;
    up.mouse_valid = true;
    up.mouse_fb = {10, 10};
    t.frame(1, up);

    // A notch binds the wheel and does not scroll.
    map.begin_capture(jump, 0, 2);
    RawInput notch;
    notch.wheel = 1;
    t.frame(1, notch);
    CHECK(map.bindings(jump)[2] == ActionMap::wheel(true));
    CHECK(t.frame_input().pointer.wheel == 0.0f);
    CHECK(t.probe().frames.back().wheel == 0.0f);

    // A key binds; its character is not typed, and a held menu key does not repeat at once.
    map.begin_capture(jump, 0, 3);
    RawInput key = test::keys_down({Key::Down});
    key.text[0] = U'x';
    key.text_count = 1;
    t.frame(1, key);
    CHECK(map.bindings(jump)[3] == ActionMap::key(Key::Down));
    CHECK(t.input().text_typed().empty());
    CHECK_FALSE(t.frame_input().is_pressed(down));
    CHECK_FALSE(t.frame_input().is_repeated(down));             // repeat timers restart from here
    CHECK(t.frame_input().is_held(down));
}

TEST_CASE("continuous scrolling presses a wheel action in every frame") {
    StepApp t;
    const ButtonAction next = t.map().button("next_weapon", {ActionMap::wheel(true)});
    RawInput r;
    r.wheel = 1;
    for (int i = 0; i < 3; ++i) {
        t.frame(1, r);
        CHECK(t.frame_input().is_pressed(next));
    }
    t.frame(1, RawInput{});
    CHECK(t.frame_input().is_released(next));
    int presses = 0;
    for (const InputFrame &f : t.probe().frames)
        presses += f.is_pressed(next) ? 1 : 0;
    CHECK(presses == 3);                                        // was 1
}

TEST_CASE("clear_touch_zones ends captures even if zones are added in the same frame") {
    StepApp t;
    ActionMap &map = t.map();
    const ButtonAction hold = map.button("hold");
    const ButtonAction brake = map.button("brake");
    map.add_touch_zone({Rect{0, 0, 200, 200}, hold, AxisAction{}});
    t.frame(1, touch_at(1, {100, 100}, true));
    REQUIRE(t.frame_input().is_held(hold));
    map.clear_touch_zones();                                    // a new HUD layout
    map.add_touch_zone({Rect{500, 300, 50, 50}, brake, AxisAction{}});
    t.frame(1, touch_at(1, {100, 100}, false));                 // the same finger, still down
    CHECK_FALSE(t.frame_input().is_held(brake));                // far outside the brake zone
    CHECK_FALSE(t.frame_input().is_held(hold));
    t.frame(1, touch_at(2, {520, 320}, true));                  // a new touch on the brake zone
    CHECK(t.frame_input().is_held(brake));
}
