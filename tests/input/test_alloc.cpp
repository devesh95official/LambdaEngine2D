// No heap allocation per frame or tick in steady state, with every input feature in use.
#include "input_test_util.h"

#include <chrono>
#include <cstdio>

using namespace lam;
using namespace lam::literals;
using lam::test::StepApp;

namespace {

struct Busy {
    std::uint64_t sum = 0;
};

void consume(Ctx &ctx) {                                        // a tick system reading everything
    const InputFrame &in = input_frame(ctx);
    Busy &b = ctx.res<Busy>();
    for (int i = 1; i < 40; ++i)
        b.sum += in.is_pressed(ButtonAction{static_cast<std::uint16_t>(i)}) ? 1u : 0u;
    b.sum += static_cast<std::uint64_t>(in.axis2(AxisAction{1}).x * 10.0f + 10.0f);
    b.sum += in.intent_count;
}

struct PassDriver final : InputDriver {
    std::uint64_t calls = 0;
    void fill(const Ctx &, std::span<InputFrame, kMaxPlayers> frames) override {
        ++calls;
        frames[1] = frames[0];
    }
};

RawInput busy_raw(int i) {
    RawInput r;
    for (int k = 0; k < 20; ++k)
        r.set_key(static_cast<Key>(static_cast<int>(Key::A) + k), ((i + k) % 3) == 0);
    r.mouse_valid = true;
    r.mouse_fb = {static_cast<float>(i % 640), 100};
    r.mouse_down = static_cast<std::uint8_t>(i % 2);
    r.wheel = (i % 9 == 0) ? 1.0f : 0.0f;
    r.pads[0].connected = true;
    r.pads[0].buttons = (i % 4 == 0) ? (1u << 7) : 0u;
    r.pads[0].axes[0] = static_cast<float>(i % 10) / 10.0f;
    if (i % 5 < 3) {
        r.touch_count = 2;
        r.touches[0] = RawTouch{1, {50, 50}, i % 5 == 0};
        r.touches[1] = RawTouch{2 + i % 7, {300, 200}, false};
    }
    r.text_count = 2;
    r.text[0] = U'a';
    r.text[1] = U'b';
    return r;
}

void define_busy_map(ActionMap &map) {
    for (int k = 0; k < 20; ++k)
        map.button("k" + std::to_string(k), {ActionMap::key(static_cast<Key>(static_cast<int>(Key::A) + k))});
    const ButtonAction rep = map.button("rep", {ActionMap::key(Key::A), ActionMap::pad(PadButton::South)});
    map.set_repeat(rep, 4, 2);
    map.button("click", {ActionMap::mouse(MouseButton::Left), ActionMap::wheel(true)});
    const AxisAction move = map.axis2("move", {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A),
                                                ActionMap::key(Key::D)}},
                                      {{PadAxis::LeftX, PadAxis::LeftY}});
    map.axis1("zoom", {ActionMap::pad_axis(PadAxis::RT, true, 0.1f)}, {ActionMap::pad_axis(PadAxis::LT, true, 0.1f)});
    map.add_touch_zone({Rect{0, 0, 100, 100}, ButtonAction{}, move});
    map.add_touch_zone({Rect{250, 150, 100, 100}, rep, AxisAction{}});
    map.rebind(rep, 1, 0, ActionMap::key(Key::Enter));
}

} // namespace

TEST_CASE("alloc: input frame and tick paths do not allocate (0/1/4-tick frames, recording, driver)") {
    test::TempDir dir;
    StepApp t;
    define_busy_map(t.map());
    t.app().insert_resource<Busy>();
    t.app().add_system(Stage::Input, &consume, "test.consume");
    PassDriver driver;
    t.input().set_driver(&driver);
    InputRecorder rec;
    REQUIRE(rec.open(dir.file("alloc.lamr"), current_replay_header(1, 64, 2)));
    t.input().set_recorder(&rec);
    t.probe().frames.reserve(4096);
    t.probe().p1.reserve(4096);

    auto run = [&](int frames, int offset) {
        for (int i = 0; i < frames; ++i) {
            t.input().feed_raw(busy_raw(offset + i));
            if (i % 3 == 0) t.input().queue_intent({"tick"_id, i, 0});
            t.frame(i % 6 == 0 ? 0 : (i % 6 == 1 ? 4 : 1));
        }
    };
    run(120, 0);                                                // warm-up (settings mirror, first flush)
    CHECK_NO_ALLOC(run(600, 120));
    CHECK(driver.calls > 600);
    CHECK(rec.ticks_recorded() > 600);
    t.input().set_recorder(nullptr);
    t.input().set_driver(nullptr);
    REQUIRE(rec.close());
}

TEST_CASE("alloc: HeadlessApp steady state with InputPlugin (run_ticks 120 then 600)") {
    test::HeadlessApp h;
    h.add<InputPlugin>();
    define_busy_map(h.app().res<ActionMap>());
    h.app().insert_resource<Busy>();
    h.app().add_system(Stage::Input, &consume, "test.consume");
    test::run_ticks(h.app(), 120);
    CHECK_NO_ALLOC(test::run_ticks(h.app(), 600));
}

TEST_CASE("bench: input frame + publish cost with 255 actions and 4 players" * doctest::skip(false)) {
    StepApp t;
    ActionMap &map = t.map();
    for (int i = 0; i < 255; ++i) {
        const ButtonAction a = map.button("a" + std::to_string(i), {ActionMap::key(static_cast<Key>(static_cast<int>(Key::A) + i % 26)),
                                                                   ActionMap::pad(PadButton::South)});
        for (int p = 1; p < kMaxPlayers; ++p)
            map.rebind(a, p, 0, ActionMap::pad(static_cast<PadButton>(1 + (i % 17)), p));
    }
    for (int i = 0; i < 15; ++i)
        map.axis2("ax" + std::to_string(i), {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A),
                                              ActionMap::key(Key::D)}}, {{PadAxis::LeftX, PadAxis::LeftY}});
    t.probe().frames.reserve(2100);
    t.probe().p1.reserve(2100);
    for (int i = 0; i < 60; ++i)
        t.frame(1, busy_raw(i));
    const auto start = std::chrono::steady_clock::now();
    constexpr int kFrames = 2000;
    for (int i = 0; i < kFrames; ++i)
        t.frame(1, busy_raw(i));
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kFrames;
    MESSAGE("input: frame with 1 tick, 255 actions x 4 players + 15 axes: " << us << " us/frame (whole App frame)");
    std::printf("[bench] input frame+tick, 255 actions x 4 players: %.2f us/frame\n", us);
    CHECK(us < 1000.0);
}
