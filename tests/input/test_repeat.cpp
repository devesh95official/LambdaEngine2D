// Key repeat: InputFrame::is_repeated counts ticks, FrameInput::is_repeated counts real time.
#include "input_test_util.h"

using namespace lam;
using lam::test::StepApp;

TEST_CASE("held menu keys repeat after a delay") {
    // At 64 Hz: delay 19 ticks (~0.3 s), interval 6 ticks (~0.1 s).
    StepApp t;
    const ButtonAction down = t.map().button("ui.down", {ActionMap::key(Key::Down)});
    t.map().set_repeat(down, 19, 6);
    t.frame(1, test::keys_down({Key::Down}));
    CHECK(t.frame_input().is_repeated(down));                   // the press itself
    int repeats = 0;
    for (int i = 0; i < 64; ++i) {                              // one second held, 1 tick per frame
        t.frame(1, test::keys_down({Key::Down}));
        if (t.frame_input().is_repeated(down)) ++repeats;
    }
    CHECK(repeats >= 5);
    CHECK(repeats <= 8);
    t.frame(1, RawInput{});
    CHECK_FALSE(t.frame_input().is_repeated(down));
    CHECK_FALSE(t.frame_input().is_held(down));
}

TEST_CASE("repeat in ticks: press, then delay, then every interval; exact and frame-pacing independent") {
    for (int ticks_per_frame : {1, 2, 4}) {
        CAPTURE(ticks_per_frame);
        StepApp t;
        const ButtonAction a = t.map().button("move", {ActionMap::key(Key::Right)});
        t.map().set_repeat(a, 10, 5);
        for (int i = 0; i < 32 / ticks_per_frame; ++i)
            t.frame(ticks_per_frame, test::keys_down({Key::Right}));
        const auto &frames = t.probe().frames;
        REQUIRE(frames.size() == 32);
        std::vector<int> at;
        for (std::size_t i = 0; i < frames.size(); ++i)
            if (frames[i].is_repeated(a)) at.push_back(static_cast<int>(i));
        CHECK(at == std::vector<int>{0, 10, 15, 20, 25, 30});
        CHECK(frames[0].is_pressed(a));
        CHECK_FALSE(frames[10].is_pressed(a));
    }
}

TEST_CASE("repeat: interval 0 means no repeat; delay 0 repeats every interval; release restarts") {
    StepApp t;
    const ButtonAction plain = t.map().button("plain", {ActionMap::key(Key::P)});
    const ButtonAction fast = t.map().button("fast", {ActionMap::key(Key::F)});
    t.map().set_repeat(fast, 0, 3);
    CHECK(t.map().repeat_delay(fast) == 0);
    CHECK(t.map().repeat_interval(fast) == 3);
    for (int i = 0; i < 10; ++i)
        t.frame(1, test::keys_down({Key::P, Key::F}));
    const auto &frames = t.probe().frames;
    int plain_repeats = 0;
    std::vector<int> fast_at;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        plain_repeats += frames[i].is_repeated(plain) ? 1 : 0;
        if (frames[i].is_repeated(fast)) fast_at.push_back(static_cast<int>(i));
    }
    CHECK(plain_repeats == 1);                                  // only the press
    CHECK(fast_at == std::vector<int>{0, 3, 6, 9});
    t.frame(1, RawInput{});
    t.frame(1, test::keys_down({Key::F}));
    t.frame(1, test::keys_down({Key::F}));
    REQUIRE(frames.size() == 13);
    CHECK(frames[11].is_repeated(fast));                        // new press
    CHECK_FALSE(frames[12].is_repeated(fast));                  // the timer restarted
}

TEST_CASE("repeat: FrameInput keeps repeating while ticks are held (pause menus)") {
    StepApp t;
    const ButtonAction down = t.map().button("ui.down", {ActionMap::key(Key::S)});
    t.map().set_repeat(down, 8, 4);                             // 0.125 s, then every 0.0625 s
    t.app().set_paused(true);
    int repeats = 0;
    for (int i = 0; i < 32; ++i) {                              // half a second of 1/64 s frames
        t.frame(1, test::keys_down({Key::S}));
        if (t.frame_input().is_repeated(down)) ++repeats;
    }
    CHECK(t.probe().frames.empty());                            // no tick ran
    // press + repeats at 8, 12, 16, 20, 24, 28 frames after it (frames 1..31 measure 1/64 s each)
    CHECK(repeats == 7);
}
