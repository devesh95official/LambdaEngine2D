// The input latch. Devices are read once per frame, a frame runs 0..N ticks, and every press must
// reach exactly one tick.
#include "input_test_util.h"

#include <lam/core/app.h>

using namespace lam;
using lam::test::Probe;
using lam::test::StepApp;

namespace {

int count_pressed(const std::vector<InputFrame> &frames, ButtonAction a) {
    int n = 0;
    for (const InputFrame &f : frames)
        n += f.is_pressed(a) ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("action latch: each press reaches exactly one tick") {
    StepApp t;
    const ButtonAction jump = t.map().button("jump", {ActionMap::key(Key::Space)});
    Probe &probe = t.probe();

    // A frame with no tick: the press waits.
    t.frame(0, test::keys_down({Key::Space}));
    CHECK(probe.frames.empty());
    // Released before any tick ran: the press still reaches the next tick, with its release.
    t.frame(0, RawInput{});
    t.frame(4, RawInput{});
    REQUIRE(probe.frames.size() == 4);
    CHECK(probe.frames[0].is_pressed(jump));
    CHECK(probe.frames[0].is_released(jump));
    CHECK_FALSE(probe.frames[0].is_held(jump));
    // The other ticks of the same frame get nothing.
    CHECK(count_pressed(probe.frames, jump) == 1);
    for (std::size_t i = 1; i < 4; ++i) {
        CHECK_FALSE(probe.frames[i].is_pressed(jump));
        CHECK_FALSE(probe.frames[i].is_released(jump));
    }
}

TEST_CASE("latch: 1-tick and 4-tick frames, held is shared by every tick of a frame") {
    StepApp t;
    const ButtonAction fire = t.map().button("fire", {ActionMap::key(Key::F)});
    Probe &probe = t.probe();

    t.frame(4, test::keys_down({Key::F}));
    REQUIRE(probe.frames.size() == 4);
    CHECK(count_pressed(probe.frames, fire) == 1);
    CHECK(probe.frames[0].is_pressed(fire));
    for (const InputFrame &f : probe.frames)
        CHECK(f.is_held(fire));                                 // held for every tick of the frame

    t.frame(1, test::keys_down({Key::F}));                       // still held: no new press
    REQUIRE(probe.frames.size() == 5);
    CHECK_FALSE(probe.frames[4].is_pressed(fire));
    CHECK(probe.frames[4].is_held(fire));

    t.frame(1, RawInput{});                                      // released
    REQUIRE(probe.frames.size() == 6);
    CHECK(probe.frames[5].is_released(fire));
    CHECK_FALSE(probe.frames[5].is_held(fire));

    // Press, hold over several 0-tick frames, then a 1-tick frame: one press, still held.
    t.frame(0, test::keys_down({Key::F}));
    t.frame(0, test::keys_down({Key::F}));
    t.frame(0, test::keys_down({Key::F}));
    t.frame(1, test::keys_down({Key::F}));
    REQUIRE(probe.frames.size() == 7);
    CHECK(probe.frames[6].is_pressed(fire));
    CHECK(probe.frames[6].is_held(fire));
    CHECK_FALSE(probe.frames[6].is_released(fire));
}

TEST_CASE("latch: two presses before a tick count once, a press in each frame reaches each tick") {
    StepApp t;
    const ButtonAction a = t.map().button("a", {ActionMap::key(Key::A)});
    Probe &probe = t.probe();
    t.frame(0, test::keys_down({Key::A}));
    t.frame(0, RawInput{});
    t.frame(0, test::keys_down({Key::A}));                      // pressed twice before a tick
    t.frame(1, RawInput{});
    REQUIRE(probe.frames.size() == 1);
    CHECK(probe.frames[0].is_pressed(a));
    CHECK(probe.frames[0].is_released(a));

    for (int i = 0; i < 6; ++i)                                 // tap every frame, 1 tick each
        t.frame(1, (i % 2 == 0) ? test::keys_down({Key::A}) : RawInput{});
    CHECK(count_pressed(probe.frames, a) == 1 + 3);
}

TEST_CASE("latch: a press while paused never reaches a tick; reset_latch forgets pending presses") {
    StepApp t;
    const ButtonAction pause = t.map().button("pause", {ActionMap::key(Key::Escape)});
    const ButtonAction jump = t.map().button("jump", {ActionMap::key(Key::Space)});
    Probe &probe = t.probe();

    t.app().set_paused(true);
    t.frame(4, test::keys_down({Key::Escape}));                  // paused: 0 ticks
    CHECK(probe.frames.empty());
    CHECK(t.frame_input().is_pressed(pause));                   // the menu still sees it (frame time)
    t.app().set_paused(false);                                   // e.g. the menu unpauses on Escape
    t.frame(1, RawInput{});
    REQUIRE(probe.frames.size() == 1);
    CHECK_FALSE(probe.frames[0].is_pressed(pause));             // the unpause key does not re-pause

    t.frame(0, test::keys_down({Key::Space}));
    t.input().reset_latch();
    t.frame(1, test::keys_down({Key::Space}));
    REQUIRE(probe.frames.size() == 2);
    CHECK_FALSE(probe.frames[1].is_pressed(jump));              // forgotten
    CHECK(probe.frames[1].is_held(jump));                       // held state is unaffected
}

TEST_CASE("latch: a focus loss (LifecycleChanged) resets pending presses") {
    StepApp t;
    const ButtonAction jump = t.map().button("jump", {ActionMap::key(Key::Space)});
    Probe &probe = t.probe();
    t.frame(0, test::keys_down({Key::Space}));
    t.app().events().get<LifecycleChanged>().emit(LifecycleChanged{LifecycleEvent::FocusLost});
    t.frame(1, RawInput{});
    REQUIRE(probe.frames.size() == 1);
    CHECK_FALSE(probe.frames[0].is_pressed(jump));
}

TEST_CASE("latch: per-player bits; unfocused worlds read neutral frames") {
    StepApp t;
    ActionMap &map = t.map();
    const ButtonAction act = map.button("act", {ActionMap::key(Key::E)});
    map.rebind(act, 1, 0, ActionMap::key(Key::Enter));
    Probe &probe = t.probe();
    t.frame(1, test::keys_down({Key::Enter}));
    REQUIRE(probe.frames.size() == 1);
    CHECK_FALSE(probe.frames[0].is_pressed(act));
    CHECK(probe.p1[0].is_pressed(act));

    // A world without input focus reads the neutral frame.
    World &other = t.app().create_world("other");
    const WorldEntry stack[] = {{&t.app().main_world(), true, true, true}, {&other, true, true, false}};
    t.app().set_world_stack(stack);
    t.frame(1, test::keys_down({Key::E}));
    // Two worlds ticked: main (focused) and other (not focused).
    REQUIRE(probe.frames.size() == 3);
    const int pressed = (probe.frames[1].is_pressed(act) ? 1 : 0) + (probe.frames[2].is_pressed(act) ? 1 : 0);
    CHECK(pressed == 1);
}

TEST_CASE("latch: a focused-world change forgets the press that caused it") {
    StepApp t;
    const ButtonAction confirm = t.map().button("confirm", {ActionMap::key(Key::Enter)});
    Probe &probe = t.probe();
    World &game = t.app().create_world("game");
    t.frame(0, test::keys_down({Key::Enter}));                  // menu confirms (no tick ran)
    const WorldEntry stack[] = {{&game, true, true, true}};
    t.app().set_world_stack(stack);                             // the menu switches to the game
    t.frame(1, test::keys_down({Key::Enter}));
    REQUIRE(probe.frames.size() == 1);
    CHECK_FALSE(probe.frames[0].is_pressed(confirm));
    CHECK(probe.frames[0].is_held(confirm));
}

TEST_CASE("latch: 255 button actions and 15 axes, ids never alias") {
    ActionMap map;
    std::vector<ButtonAction> ids;
    for (int i = 0; i < 255; ++i)
        ids.push_back(map.button("b" + std::to_string(i), {ActionMap::key(static_cast<Key>(static_cast<int>(Key::A) + i % 26))}));
    CHECK(ids.front().id == 1);
    CHECK(ids.back().id == 255);
    CHECK(map.button_count() == 255);
    for (int i = 0; i < 15; ++i)
        CHECK(map.axis2("x" + std::to_string(i)).id == i + 1);
    CHECK(map.axis_count() == 15);

    InputFrame f;
    f.pressed[3] = std::uint64_t{1} << 63;                      // id 255
    CHECK(f.is_pressed(ButtonAction{255}));
    CHECK_FALSE(f.is_pressed(ButtonAction{0}));                 // none is never pressed
    CHECK_FALSE(f.is_pressed(ButtonAction{256}));               // out of range
    CHECK(f.axis2(AxisAction{16}) == Vec2{});
    static_assert(!std::is_convertible_v<AxisAction, ButtonAction>);
    static_assert(!std::is_convertible_v<ButtonAction, AxisAction>);

    // The 255th action latches like the first.
    StepApp t;
    for (int i = 0; i < 254; ++i)
        t.map().button("pad" + std::to_string(i));
    const ButtonAction last = t.map().button("last", {ActionMap::key(Key::Z)});
    REQUIRE(last.id == 255);
    t.frame(1, test::keys_down({Key::Z}));
    REQUIRE(t.probe().frames.size() == 1);
    CHECK(t.probe().frames[0].is_pressed(last));
}
