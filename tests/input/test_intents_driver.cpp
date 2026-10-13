// Intents reach exactly one tick. InputDriver runs at TickBegin -1400, after the latch is
// published and before any tick stage.
#include "input_test_util.h"

#include <lam/ecs/world.h>

using namespace lam;
using namespace lam::literals;
using lam::test::StepApp;

namespace {

int count_intents(const std::vector<InputFrame> &frames, StrId kind) {
    int n = 0;
    for (const InputFrame &f : frames)
        for (const Intent &i : f.intent_list())
            n += i.kind == kind ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("intents: delivered in the next tick exactly once, in order, per player") {
    StepApp t;
    InputSystem &in = t.input();
    in.queue_intent({"use_item"_id, 3, 0});
    in.queue_intent({"drop"_id, 4, 5});
    in.queue_intent({"buy"_id, 9, 0}, 2);
    t.frame(0);                                                 // no tick: they wait
    CHECK(t.probe().frames.empty());
    t.frame(4);
    const auto &frames = t.probe().frames;
    REQUIRE(frames.size() == 4);
    REQUIRE(frames[0].intent_count == 2);
    CHECK(frames[0].intent_list()[0].kind == "use_item"_id);
    CHECK(frames[0].intent_list()[0].a == 3);
    CHECK(frames[0].intent_list()[1].kind == "drop"_id);
    CHECK(frames[0].intent_list()[1].b == 5);
    for (std::size_t i = 1; i < 4; ++i)
        CHECK(frames[i].intent_count == 0);
    CHECK(count_intents(frames, "use_item"_id) == 1);
    CHECK(t.app().res<ActionState>().players[2].intent_count == 0);   // player 2 consumed at tick 1
}

TEST_CASE("intents: more than kMaxIntents per tick are dropped and counted; they survive pause") {
    StepApp t;
    InputSystem &in = t.input();
    for (int i = 0; i < kMaxIntents + 2; ++i)
        in.queue_intent({"x"_id, i, 0});
    CHECK(in.dropped_intents() == 2);
    t.app().set_paused(true);
    t.frame(2);
    t.frame(2);
    CHECK(t.probe().frames.empty());
    t.app().set_paused(false);
    t.frame(1);
    REQUIRE(t.probe().frames.size() == 1);
    CHECK(t.probe().frames[0].intent_count == kMaxIntents);     // reset_latch keeps intents
    CHECK(t.probe().frames[0].intent_list()[3].a == 3);
}

namespace {

struct Bot final : InputDriver {
    ButtonAction jump;
    std::uint64_t calls = 0;
    std::uint64_t last_tick = 0;
    std::uint32_t world_id = 0;
    bool saw_live_press = false;
    void fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) override {
        ++calls;
        last_tick = ctx.app().tick();
        world_id = ctx.world().id();                            // the focused world
        saw_live_press = saw_live_press || frames[0].is_pressed(jump);
        frames[0] = InputFrame{};
        if (ctx.app().tick() % 3 == 0) {                        // press jump every third tick
            frames[0].pressed[jump.id >> 6] |= std::uint64_t{1} << (jump.id & 63u);
            frames[0].held = frames[0].pressed;
        }
    }
};

} // namespace

TEST_CASE("driver: fills after publish and before the tick stages, with the focused world") {
    StepApp t;
    Bot bot;
    bot.jump = t.map().button("jump", {ActionMap::key(Key::Space)});
    t.input().set_driver(&bot);
    CHECK(t.input().driver() == &bot);
    t.frame(1, test::keys_down({Key::Space}));
    CHECK(bot.saw_live_press);                                  // input_publish ran first
    for (int i = 0; i < 5; ++i)
        t.frame(1, RawInput{});
    CHECK(bot.calls == 6);
    CHECK(bot.last_tick == t.app().tick());
    CHECK(bot.world_id == t.app().main_world().id());
    const auto &frames = t.probe().frames;
    REQUIRE(frames.size() == 6);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const std::uint64_t tick = i + 1;                       // app ticks start at 1
        CHECK(frames[i].is_pressed(bot.jump) == (tick % 3 == 0));
    }
    t.input().set_driver(nullptr);
    t.frame(1, test::keys_down({Key::Space}));
    CHECK(t.probe().frames.back().is_pressed(bot.jump));        // live devices again
}
