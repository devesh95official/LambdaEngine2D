// ActionMap: definitions, binding strings, rebinding, capture and persistence.
#include "input_test_util.h"

#include <lam/core/fs.h>
#include <lam/core/settings.h>

using namespace lam;
using lam::test::StepApp;

TEST_CASE("actions: definitions are idempotent and names resolve both ways") {
    ActionMap map;
    const ButtonAction jump = map.button("jump", {ActionMap::key(Key::Space), ActionMap::pad(PadButton::South)});
    CHECK(jump.id == 1);
    CHECK(map.button("jump", {ActionMap::key(Key::J)}) == jump);        // idempotent, defaults kept
    REQUIRE(map.bindings(jump).size() == 2);
    CHECK(map.bindings(jump)[0] == ActionMap::key(Key::Space));
    CHECK(map.find_button("jump") == jump);
    CHECK_FALSE(map.find_button("nope"));
    CHECK(map.name_of(jump) == "jump");
    CHECK(map.name_of(ButtonAction{77}).empty());
    const AxisAction move = map.axis2("move");
    CHECK(move.id == 1);
    CHECK(map.find_axis("move") == move);
    CHECK(map.name_of(move) == "move");
    CHECK(map.bindings(jump, 1).empty());                       // defaults bind player 0 only
    CHECK(map.player_count() == 1);
    // Copies are deep.
    ActionMap copy = map;
    copy.rebind(jump, 0, 0, ActionMap::key(Key::K));
    CHECK(map.bindings(jump)[0] == ActionMap::key(Key::Space));
    CHECK(copy.bindings(jump)[0] == ActionMap::key(Key::K));
}

TEST_CASE("actions: binding strings round-trip and reject garbage") {
    const Binding samples[] = {
        Binding{},
        ActionMap::key(Key::Space),
        ActionMap::key(Key::KpEnter),
        ActionMap::key(Key::F11),
        ActionMap::mouse(MouseButton::Left),
        ActionMap::mouse(MouseButton::Back),
        ActionMap::pad(PadButton::South),
        ActionMap::pad(PadButton::Start, 2),
        ActionMap::pad_axis(PadAxis::LeftX, true),
        ActionMap::pad_axis(PadAxis::RT, true, 0.25f, 1),
        ActionMap::pad_axis(PadAxis::RightY, false, 0.125f),
        ActionMap::wheel(true),
        ActionMap::wheel(false),
        ActionMap::touch(3),
    };
    for (const Binding &b : samples) {
        const std::string s = binding_to_string(b);
        CAPTURE(s);
        const auto back = binding_from_string(s);
        REQUIRE(back.has_value());
        CHECK(*back == b);
    }
    CHECK(binding_to_string(ActionMap::key(Key::Space)) == "key:space");
    CHECK(binding_to_string(ActionMap::mouse(MouseButton::Left)) == "mouse:left");
    CHECK(binding_to_string(ActionMap::pad(PadButton::South)) == "pad:south");
    CHECK(binding_to_string(ActionMap::pad_axis(PadAxis::LeftX, true)) == "axis:+left_x");
    CHECK(binding_to_string(ActionMap::pad_axis(PadAxis::RT, false, 0.25f, 1)) == "axis:-rt@1>0.25");
    CHECK(binding_to_string(Binding{}) == "none");
    CHECK(binding_to_string(Binding{BindingSource::Key, 3, -1, 0.5f}) == "key:#3");
    CHECK(binding_from_string("key:#3")->code == 3);
    CHECK(binding_from_string("  KEY:Space ") == ActionMap::key(Key::Space));   // case and spaces
    for (const char *bad : {"", "key", "key:", "key:nokey", "key:#0", "key:#999", "mouse:middle_ish",
                            "pad:unknown", "pad:south@9", "pad:south>0.2", "axis:left_x", "axis:+nope",
                            "axis:+rt>1.5", "axis:+rt>x", "wheel:sideways", "touch:99", "joy:1"}) {
        CAPTURE(bad);
        CHECK_FALSE(binding_from_string(bad).has_value());
    }
}

TEST_CASE("actions: rebind, clear, reset to defaults; slots keep their positions") {
    ActionMap map;
    const ButtonAction fire = map.button("fire", {ActionMap::key(Key::F)});
    const std::uint32_t r0 = map.revision();
    map.rebind(fire, 0, 2, ActionMap::mouse(MouseButton::Left));
    CHECK(map.revision() != r0);
    auto b = map.bindings(fire);
    REQUIRE(b.size() == 3);
    CHECK(b[0] == ActionMap::key(Key::F));
    CHECK(b[1].source == BindingSource::None);
    CHECK(b[2] == ActionMap::mouse(MouseButton::Left));
    map.rebind(fire, 0, 2, Binding{});                          // None clears the slot
    CHECK(map.bindings(fire).size() == 1);
    map.clear_bindings(fire, 0);
    CHECK(map.bindings(fire).empty());
    map.rebind(fire, 3, 0, ActionMap::pad(PadButton::RB));
    CHECK(map.player_count() == 4);
    map.reset_bindings(0);
    REQUIRE(map.bindings(fire).size() == 1);
    CHECK(map.bindings(fire)[0] == ActionMap::key(Key::F));
    map.reset_bindings(3);
    CHECK(map.bindings(fire, 3).empty());
    CHECK(map.player_count() == 1);

    const AxisAction move = map.axis2("move", {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A),
                                                ActionMap::key(Key::D)}});
    map.rebind(move, 0, 0, AxisDir::Up, ActionMap::key(Key::Z));
    REQUIRE(map.axis_bindings(move).size() == 1);
    CHECK(map.axis_bindings(move)[0].up == ActionMap::key(Key::Z));
    CHECK(map.axis_bindings(move)[0].down == ActionMap::key(Key::S));
    map.clear_bindings(move, 0);
    CHECK(map.axis_bindings(move).empty());
    map.reset_bindings(0);
    CHECK(map.axis_bindings(move)[0].up == ActionMap::key(Key::W));
}

TEST_CASE("actions: capture-next-input rebinds a slot; the captured press does not act; Escape cancels") {
    StepApp t;
    ActionMap &map = t.map();
    const ButtonAction jump = map.button("jump", {ActionMap::key(Key::Space)});
    const ButtonAction other = map.button("other", {ActionMap::key(Key::K)});
    t.frame(1, test::keys_down({Key::Enter}));                  // the menu's confirm key is held
    map.begin_capture(jump, 0, 1);
    CHECK(map.capturing());
    t.frame(1, test::keys_down({Key::Enter}));                  // held, not new: nothing captured
    CHECK(map.capturing());
    t.frame(1, test::keys_down({Key::Enter, Key::K}));          // K is new: captured
    CHECK_FALSE(map.capturing());
    REQUIRE(map.bindings(jump).size() == 2);
    CHECK(map.bindings(jump)[1] == ActionMap::key(Key::K));
    CHECK_FALSE(t.frame_input().is_pressed(other));             // suppressed while capturing
    t.frame(1, test::keys_down({Key::K}));
    CHECK_FALSE(t.frame_input().is_pressed(jump));              // still held: no new press
    t.frame(1, RawInput{});
    t.frame(1, test::keys_down({Key::K}));
    CHECK(t.frame_input().is_pressed(jump));
    CHECK(t.frame_input().is_pressed(other));
    for (const InputFrame &f : t.probe().frames)                // no tick saw K pressed during capture
        CHECK(f.is_pressed(other) == (&f == &t.probe().frames.back()));

    // Mouse, wheel, pad button and pad axis captures; Escape cancels.
    map.begin_capture(jump, 0, 2);
    t.frame(1, RawInput{});
    RawInput m;
    m.mouse_down = 1u << static_cast<unsigned>(MouseButton::Middle);
    t.frame(1, m);
    CHECK(map.bindings(jump)[2] == ActionMap::mouse(MouseButton::Middle));
    map.begin_capture(jump, 0, 2);
    RawInput w;
    w.wheel = -1;
    t.frame(1, w);
    CHECK(map.bindings(jump)[2] == ActionMap::wheel(false));
    map.begin_capture(jump, 0, 3);
    t.frame(1, test::with_pad({}, 1, {PadButton::West}));
    CHECK(map.bindings(jump)[3] == ActionMap::pad(PadButton::West));
    map.begin_capture(jump, 0, 3);
    t.frame(1, test::with_pad({}, 1, {PadButton::West}, {{PadAxis::LeftY, -0.9f}}));
    CHECK(map.bindings(jump)[3] == ActionMap::pad_axis(PadAxis::LeftY, false));
    map.begin_capture(jump, 0, 0);
    t.frame(1, test::keys_down({Key::Escape}));
    CHECK_FALSE(map.capturing());
    CHECK(map.bindings(jump)[0] == ActionMap::key(Key::Space));  // unchanged

    // Axis capture of one direction.
    const AxisAction move = map.axis2("move", {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A),
                                                ActionMap::key(Key::D)}});
    map.begin_capture(move, 0, 0, AxisDir::Left);
    t.frame(1, RawInput{});
    t.frame(1, test::keys_down({Key::Q}));
    CHECK(map.axis_bindings(move)[0].left == ActionMap::key(Key::Q));
    map.cancel_capture();
    CHECK_FALSE(map.capturing());
}

TEST_CASE("actions: settings round trip (store and file); invalid entries keep defaults") {
    test::TempDir dir;
    const std::string path = dir.file("settings.json");
    auto define = [](ActionMap &map) {
        map.button("jump", {ActionMap::key(Key::Space), ActionMap::pad(PadButton::South)});
        map.button("fire", {ActionMap::mouse(MouseButton::Left)});
        map.axis2("move", {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A), ActionMap::key(Key::D)}},
                  {{PadAxis::LeftX, PadAxis::LeftY}});
        map.axis1("zoom", {ActionMap::wheel(true)}, {ActionMap::wheel(false)});
    };
    {
        ActionMap map;
        define(map);
        map.rebind(map.find_button("jump"), 0, 0, ActionMap::key(Key::J));
        map.rebind(map.find_button("jump"), 0, 3, ActionMap::pad_axis(PadAxis::RT, true, 0.3f));
        map.clear_bindings(map.find_button("fire"), 0);
        map.rebind(map.find_axis("move"), 0, 0, AxisDir::Up, ActionMap::key(Key::Up));
        map.rebind(map.find_button("fire"), 1, 0, ActionMap::pad(PadButton::RB));
        SettingsStore s;
        REQUIRE(map.save(s));
        CHECK(s.get_string("input.p0.jump.0") == "key:j");
        CHECK(s.get_string("input.p0.jump.1") == "pad:south");
        CHECK(s.get_string("input.p0.jump.2") == "none");
        CHECK(s.get_string("input.p0.jump.3") == "axis:+rt>0.3");
        CHECK(s.get_string("input.p0.fire.0") == "none");
        CHECK(s.get_string("input.p0.move.0") == "key:up,key:s,key:a,key:d");
        CHECK(s.get_string("input.p0.zoom.0") == "wheel:down,wheel:up");
        CHECK(s.get_string("input.p1.fire.0") == "pad:rb");
        REQUIRE(s.save(path));
    }
    {
        SettingsStore s;
        REQUIRE(s.load(path));                                  // before the keys are defined
        ActionMap map;
        define(map);
        map.assign_player(1, 1, false);                         // the game sets up player 1 again
        map.define_settings(s);
        REQUIRE(map.load(s));
        const ButtonAction jump = map.find_button("jump");
        REQUIRE(map.bindings(jump).size() == 4);
        CHECK(map.bindings(jump)[0] == ActionMap::key(Key::J));
        CHECK(map.bindings(jump)[3] == ActionMap::pad_axis(PadAxis::RT, true, 0.3f));
        CHECK(map.bindings(map.find_button("fire")).empty());
        CHECK(map.axis_bindings(map.find_axis("move"))[0].up == ActionMap::key(Key::Up));
        CHECK(map.stick_bindings(map.find_axis("move")).size() == 1);  // sticks are not persisted
        REQUIRE(map.bindings(map.find_button("fire"), 1).size() == 1);
        CHECK(map.bindings(map.find_button("fire"), 1)[0] == ActionMap::pad(PadButton::RB));
    }
    {
        // Hand-edited garbage keeps the defaults; unknown keys are kept for other modules.
        REQUIRE(write_file_atomic(path, R"({"input.p0.jump.0": "key:nonsense", "input.p0.move.0": "key:w,key:s",
                                           "input.p0.fire.0": 42, "input.p0.fire.1": "mouse:right", "other.key": 1})"));
        SettingsStore s;
        REQUIRE(s.load(path));
        ActionMap map;
        define(map);
        map.define_settings(s);
        test::CaptureLog log;
        REQUIRE(map.load(s));
        CHECK(log.contains("input.p0.jump.0"));
        CHECK(map.bindings(map.find_button("jump"))[0] == ActionMap::key(Key::Space));
        CHECK(map.axis_bindings(map.find_axis("move"))[0].up == ActionMap::key(Key::W));
        REQUIRE(map.bindings(map.find_button("fire")).size() == 2);
        CHECK(map.bindings(map.find_button("fire"))[0] == ActionMap::mouse(MouseButton::Left));
        CHECK(map.bindings(map.find_button("fire"))[1] == ActionMap::mouse(MouseButton::Right));
    }
}

TEST_CASE("actions: InputPlugin loads bindings at finish and mirrors rebinds into SettingsStore") {
    StepApp t;                                                  // finish already ran (first frame)
    ActionMap &map = t.map();
    const ButtonAction jump = map.button("jump", {ActionMap::key(Key::Space)});
    map.rebind(jump, 0, 0, ActionMap::key(Key::H));
    t.frame(0);
    SettingsStore &s = t.app().res<SettingsStore>();
    REQUIRE(s.defined("input.p0.jump.0"));
    CHECK(s.get_string("input.p0.jump.0") == "key:h");

    // A fresh App whose SettingsStore already holds a binding applies it at finish().
    const char *const argv[] = {"lam_input_tests", "--headless"};
    App app(test::StepApp::make_config(), 2, argv);
    app.add_plugin<InputPlugin>();
    app.res<SettingsStore>().define_string("input.p0.jump.0", "key:space");
    app.res<SettingsStore>().set_string("input.p0.jump.0", "key:g");
    const ButtonAction j2 = app.res<ActionMap>().button("jump", {ActionMap::key(Key::Space)});
    app.run_frames(1);
    CHECK(app.res<ActionMap>().bindings(j2)[0] == ActionMap::key(Key::G));
}

TEST_CASE("actions: actions defined after finish still get their saved bindings; in-memory rebinds win") {
    const char *const argv[] = {"lam_input_tests", "--headless"};
    App app(test::StepApp::make_config(), 2, argv);
    app.add_plugin<InputPlugin>();
    SettingsStore &s = app.res<SettingsStore>();
    // As if loaded from settings.json before anything was defined.
    s.define_string("input.p0.late.0", "key:space");
    s.set_string("input.p0.late.0", "key:l");
    s.define_string("input.p0.moved.0", "key:space");
    s.set_string("input.p0.moved.0", "key:m");
    app.run_frames(1);                                          // finish: no actions yet
    ActionMap &map = app.res<ActionMap>();
    const ButtonAction late = map.button("late", {ActionMap::key(Key::Space)});   // e.g. in a startup system
    const ButtonAction moved = map.button("moved", {ActionMap::key(Key::Space)});
    map.rebind(moved, 0, 0, ActionMap::key(Key::N));            // the player rebinds before the next frame
    app.run_frames(1);
    CHECK(map.bindings(late)[0] == ActionMap::key(Key::L));     // saved value applied
    CHECK(map.bindings(moved)[0] == ActionMap::key(Key::N));    // the rebind is not undone
    CHECK(s.get_string("input.p0.moved.0") == "key:n");         // and is mirrored
    CHECK(s.get_string("input.p0.late.0") == "key:l");
}
