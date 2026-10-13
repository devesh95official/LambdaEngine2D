// Input recording and replay.
#include "input_test_util.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <lam/core/build_info.h>
#include <lam/core/fs.h>
#include <lam/ecs/component.h>
#include <lam/ecs/world.h>

struct ReplayMover {
    float x = 0;
    float y = 0;
    std::int32_t score = 0;
};
LAM_REFLECT(ReplayMover, x, y, score)

using namespace lam;
using namespace lam::literals;

namespace {

struct SimControls {
    AxisAction move;
    ButtonAction jump;
};

struct Hashes {
    std::vector<std::uint64_t> per_tick;
};

void sim_spawn(Ctx &ctx) {
    World &w = ctx.world();
    for (int i = 0; i < 3; ++i) {
        const Entity e = w.create();
        w.add<ReplayMover>(e, ReplayMover{static_cast<float>(i * 10), 0, 0});
    }
}

void sim_step(Ctx &ctx) {
    const SimControls &c = ctx.res<SimControls>();
    const InputFrame &in = input_frame(ctx);
    const Vec2 v = in.axis2(c.move);
    const bool jump = in.is_pressed(c.jump);
    std::int32_t bonus = 0;
    for (const Intent &i : in.intent_list())
        bonus += static_cast<std::int32_t>(i.a);
    Rng &rng = ctx.rng("loot"_id);
    ctx.world().view<ReplayMover>().each([&](Entity, ReplayMover &m) {
        m.x += v.x * 1.5f;
        m.y += v.y * 1.5f;
        if (jump) m.score += 1 + rng.between(0, 9);             // RNG use depends on input
        m.score += bonus;
    });
}

void hash_system(Ctx &ctx) {
    ctx.res<Hashes>().per_tick.push_back(ctx.world().state_hash());
}

void setup(App &app) {
    app.add_plugin<InputPlugin>();
    app.register_component<ReplayMover>();
    ActionMap &map = app.res<ActionMap>();
    app.insert_resource<SimControls>(SimControls{
        map.axis2("move", {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A), ActionMap::key(Key::D)}},
                  {{PadAxis::LeftX, PadAxis::LeftY}}),
        map.button("jump", {ActionMap::key(Key::Space)})});
    app.insert_resource<Hashes>();
    app.add_startup(&sim_spawn, "spawn");
    app.add_system(Stage::Simulate, &sim_step, "sim");
    app.add_system({.stage = Stage::TickEnd, .fn = &hash_system, .name = "test.hash", .order = 0,
                    .scope = SystemScope::Once});
}

// Deterministic device script for frame i.
RawInput script(int i) {
    RawInput r;
    r.set_key(Key::D, i % 7 < 3);
    r.set_key(Key::W, i % 11 < 5);
    r.set_key(Key::Space, i % 5 == 0);
    r.pads[0].connected = true;
    r.pads[0].axes[0] = (i % 13 < 6) ? 0.75f : 0.0f;
    return r;
}

std::unique_ptr<App> make_app(std::vector<std::string> args) {
    static std::vector<std::string> storage;                    // argv must outlive parsing only
    storage = std::move(args);
    std::vector<const char *> argv;
    for (const std::string &s : storage)
        argv.push_back(s.c_str());
    auto app = std::make_unique<App>(test::StepApp::make_config(), static_cast<int>(argv.size()), argv.data());
    setup(*app);
    return app;
}

// Runs `frames` frames of the script (one tick each under --headless) and returns the hashes.
std::vector<std::uint64_t> run_scripted(App &app, int frames, bool feed) {
    for (int i = 0; i < frames; ++i) {
        if (feed) {
            app.res<InputSystem>().feed_raw(script(i));
            if (i % 17 == 4) app.res<InputSystem>().queue_intent({"gift"_id, i, 0});
        } else {
            app.res<InputSystem>().feed_raw(script(i * 3 + 1));  // live garbage: must be ignored
        }
        app.run_frames(1);
    }
    return app.res<Hashes>().per_tick;
}

} // namespace

TEST_CASE("replay: a recorded run replays to the same state_hash at every tick") {
    test::TempDir dir;
    const std::string path = dir.file("run.lamr");
    std::vector<std::uint64_t> recorded;
    std::int32_t recorded_score = 0;
    {
        auto app = make_app({"t", "--headless", "--record-input", path, "--seed", "77"});
        recorded = run_scripted(*app, 200, true);
        app->main_world().view<ReplayMover>().each([&](Entity, ReplayMover &m) { recorded_score += m.score; });
        REQUIRE(app->res<InputSystem>().recorder() != nullptr);
        CHECK(app->res<InputSystem>().recorder()->ticks_recorded() == 200);
    }                                                           // shutdown closes the recorder
    REQUIRE(recorded.size() == 200);
    CHECK(recorded_score > 0);

    auto replay = make_app({"t", "--headless", "--replay", path});
    CHECK(replay->seed() == 77);                                // header seed applied
    auto *driver = dynamic_cast<ReplayDriver *>(replay->res<InputSystem>().driver());
    REQUIRE(driver != nullptr);
    CHECK(driver->tick_count() == 200);
    CHECK(driver->first_tick() == 1);
    CHECK(driver->last_tick() == 200);
    const std::vector<std::uint64_t> replayed = run_scripted(*replay, 200, false);
    CHECK(driver->finished());
    REQUIRE(replayed.size() == recorded.size());
    std::size_t first_diff = recorded.size();
    for (std::size_t i = 0; i < recorded.size(); ++i)
        if (recorded[i] != replayed[i] && first_diff == recorded.size()) first_diff = i;
    CHECK(first_diff == recorded.size());

    // Sanity: different input gives a different history.
    auto other = make_app({"t", "--headless", "--seed", "77"});
    const std::vector<std::uint64_t> live_other = run_scripted(*other, 200, false);
    CHECK(live_other != recorded);
}

TEST_CASE("replay: the file format is the documented one and recordings are byte-identical") {
    test::TempDir dir;
    for (const char *name : {"a.lamr", "b.lamr"}) {
        auto app = make_app({"t", "--headless", "--record-input", dir.file(name)});
        (void)run_scripted(*app, 30, true);
    }
    auto a = read_file_bytes(dir.file("a.lamr"));
    auto b = read_file_bytes(dir.file("b.lamr"));
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*a == *b);

    const std::vector<std::byte> &f = *a;
    const auto u = [&](std::size_t at, int bytes) {
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i)
            v |= static_cast<std::uint64_t>(f[at + static_cast<std::size_t>(i)]) << (8 * i);
        return v;
    };
    REQUIRE(f.size() > 19);
    CHECK(std::memcmp(f.data(), "LAMR", 4) == 0);
    CHECK(u(4, 2) == 1);                                        // version
    CHECK(u(6, 2) == 64);                                       // tick rate
    CHECK(u(8, 8) == 1);                                        // seed (AppConfig::seed)
    CHECK(u(16, 1) == kMaxPlayers);                             // players: always every player
    const std::size_t id_len = u(17, 2);
    CHECK(std::string(reinterpret_cast<const char *>(f.data() + 19), id_len) == build_id());
    const std::size_t os_at = 19 + id_len;
    const std::size_t os_len = u(os_at, 2);
    CHECK(std::string(reinterpret_cast<const char *>(f.data() + os_at + 2), os_len) == os_description());
    const std::size_t body = os_at + 2 + os_len;
    CHECK(f.size() - body == 30 * (8 + kMaxPlayers * sizeof(InputFrame)));
    CHECK(u(body, 8) == 1);                                     // first record: app tick 1
    CHECK(u(body + 8 + kMaxPlayers * sizeof(InputFrame), 8) == 2);
}

TEST_CASE("replay: other builds are refused unless --replay-any-build; bad files fail cleanly") {
    test::TempDir dir;
    const std::string foreign = dir.file("foreign.lamr");
    {
        InputRecorder rec;
        ReplayHeader h = current_replay_header(5, 64, 2);
        CHECK(h.players == 2);
        h.build_id = "someone-else";
        REQUIRE(rec.open(foreign, h));
        CHECK(rec.is_open());
        std::array<InputFrame, kMaxPlayers> frames{};
        for (std::uint64_t tick = 1; tick <= 3; ++tick)
            rec.record(tick, frames);
        CHECK(rec.ticks_recorded() == 3);
        REQUIRE(rec.close());
        CHECK_FALSE(rec.is_open());
        CHECK(rec.close());                                     // closing twice is fine
    }
    {
        test::CaptureLog log;
        auto app = make_app({"t", "--headless", "--replay", foreign});
        CHECK(app->quitting());
        CHECK(app->res<InputSystem>().driver() == nullptr);
        CHECK(log.contains("refused"));
        CHECK(log.contains("someone-else"));
    }
    {
        test::CaptureLog log;
        auto app = make_app({"t", "--headless", "--replay", foreign, "--replay-any-build"});
        CHECK_FALSE(app->quitting());
        CHECK(app->res<InputSystem>().driver() != nullptr);
        CHECK(app->seed() == 5);
        CHECK(log.count(LogLevel::Warn) >= 1);
    }
    {
        ReplayHeader h = current_replay_header(5, 30, 1);
        CHECK(replay_compatible(h, 64, true).error().code == ErrorCode::Invalid);   // tick rate always matters
        CHECK(replay_compatible(h, 30, false).ok());
        h.version = 2;
        CHECK_FALSE(replay_compatible(h, 30, true).ok());
        h = current_replay_header(5, 30, 1);
        h.os = "TempleOS";
        CHECK(replay_compatible(h, 30, false).error().code == ErrorCode::Unsupported);
    }
    // Broken files.
    CHECK(ReplayDriver::open(dir.file("missing.lamr")).error().code == ErrorCode::NotFound);
    REQUIRE(write_file_atomic(dir.file("junk.lamr"), "JUNKJUNKJUNKJUNKJUNK"));
    CHECK(ReplayDriver::open(dir.file("junk.lamr")).error().code == ErrorCode::Parse);
    REQUIRE(write_file_atomic(dir.file("short.lamr"), std::string("LAMR\x01\x00", 6)));
    CHECK(ReplayDriver::open(dir.file("short.lamr")).error().code == ErrorCode::Parse);
    REQUIRE(write_file_atomic(dir.file("new.lamr"), std::string("LAMR\x09\x00", 6)));
    CHECK(ReplayDriver::open(dir.file("new.lamr")).error().code == ErrorCode::VersionTooNew);
    {
        auto bytes = read_file_bytes(foreign);
        REQUIRE(bytes);
        std::string s(reinterpret_cast<const char *>(bytes->data()), bytes->size() - 10);   // cut the last record
        REQUIRE(write_file_atomic(dir.file("cut.lamr"), s));
        test::CaptureLog log;
        auto d = ReplayDriver::open(dir.file("cut.lamr"));
        REQUIRE(d);
        CHECK((*d)->tick_count() == 2);
        CHECK((*d)->header().players == 2);
        CHECK(log.contains("partial record"));
    }
    {
        InputRecorder rec;
        ReplayHeader h = current_replay_header(1, 60, 1);
        CHECK(rec.open(dir.file("no/such/dir/x.lamr"), h).error().code == ErrorCode::Io);
        h.players = 0;
        CHECK(rec.open(dir.file("x.lamr"), h).error().code == ErrorCode::Invalid);
    }
}

TEST_CASE("replay: the driver delivers neutral frames for unrecorded ticks and after the end") {
    test::TempDir dir;
    const std::string path = dir.file("sparse.lamr");
    ButtonAction jump;
    {
        InputRecorder rec;
        REQUIRE(rec.open(path, current_replay_header(1, 64, 1)));
        ActionMap scratch;
        jump = scratch.button("jump");
        std::array<InputFrame, kMaxPlayers> frames{};
        frames[0].pressed[0] = std::uint64_t{1} << jump.id;
        frames[0].held = frames[0].pressed;
        frames[0].intents[0] = Intent{"hello"_id, 1, 2};
        frames[0].intent_count = 1;
        rec.record(2, frames);
        rec.record(4, frames);
        REQUIRE(rec.close());
    }
    auto d = ReplayDriver::open(path);
    REQUIRE(d);
    test::StepApp t;
    (void)t.map().button("jump");
    t.input().set_driver(d->get());
    for (int i = 0; i < 6; ++i)
        t.frame(1, test::keys_down({Key::Space}));
    const auto &frames = t.probe().frames;
    REQUIRE(frames.size() == 6);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const std::uint64_t tick = i + 1;
        CAPTURE(tick);
        CHECK(frames[i].is_pressed(jump) == (tick == 2 || tick == 4));
        CHECK(frames[i].intent_count == ((tick == 2 || tick == 4) ? 1 : 0));
    }
    CHECK(frames[1].intent_list()[0].kind == "hello"_id);
    CHECK((*d)->finished());
    t.input().set_driver(nullptr);
}

namespace {
void assign_coop(Ctx &ctx) {                                    // a startup system sets up player 1
    ActionMap &map = ctx.res<ActionMap>();
    map.assign_player(1, 1, false);
    map.rebind(map.find_button("jump"), 1, 0, ActionMap::pad(PadButton::South));
}
} // namespace

TEST_CASE("replay: --record-input opens at the first tick, counting players set up by startup systems") {
    test::TempDir dir;
    const std::string path = dir.file("coop.lamr");
    {
        auto app = make_app({"t", "--headless", "--record-input", path});
        app->add_startup(&assign_coop, "coop");
        for (int i = 0; i < 10; ++i) {
            RawInput r;
            r.pads[1].connected = true;
            r.pads[1].buttons = (i % 2 == 0) ? (1u << static_cast<unsigned>(PadButton::South)) : 0u;
            app->res<InputSystem>().feed_raw(r);
            app->run_frames(1);
        }
    }
    auto d = ReplayDriver::open(path);
    REQUIRE(d);
    CHECK((*d)->header().players == kMaxPlayers);              // every player is recorded
    CHECK((*d)->tick_count() == 10);
    // Player 1's presses are in the file.
    test::StepApp t;
    const ButtonAction jump = t.map().button("jump");
    t.input().set_driver(d->get());
    for (int i = 0; i < 10; ++i)
        t.frame(1);
    int presses = 0;
    for (const InputFrame &f : t.probe().p1)
        presses += f.is_pressed(jump) ? 1 : 0;
    CHECK(presses == 5);
    t.input().set_driver(nullptr);

    // An unwritable --record-input path is reported at the first tick and quits with 1.
    test::CaptureLog log;
    auto bad = make_app({"t", "--headless", "--record-input", dir.file("no/such/dir/x.lamr")});
    bad->run_frames(1);
    CHECK(bad->quitting());
    CHECK(log.contains("--record-input"));
}

namespace {
void late_join(Ctx &ctx) {                                      // player 2 joins at app tick 3
    if (ctx.app().tick() != 3) return;
    ActionMap &map = ctx.res<ActionMap>();
    map.assign_player(1, 1, false);
    map.rebind(map.find_button("jump"), 1, 0, ActionMap::pad(PadButton::South));
}
} // namespace

TEST_CASE("a player who joins after the first tick is recorded and replayed") {
    test::TempDir dir;
    const std::string path = dir.file("join.lamr");
    {
        auto app = make_app({"t", "--headless", "--record-input", path});
        app->add_system(Stage::Simulate, &late_join, "test.late_join");
        for (int i = 0; i < 10; ++i) {
            RawInput r;
            r.pads[1].connected = true;
            r.pads[1].buttons = (i % 2 == 0) ? (1u << static_cast<unsigned>(PadButton::South)) : 0u;
            app->res<InputSystem>().feed_raw(r);
            app->run_frames(1);
        }
    }
    auto d = ReplayDriver::open(path);
    REQUIRE(d);
    CHECK((*d)->header().players == kMaxPlayers);
    test::StepApp t;
    const ButtonAction jump = t.map().button("jump");
    t.input().set_driver(d->get());
    for (int i = 0; i < 10; ++i)
        t.frame(1);
    int presses = 0;
    for (const InputFrame &f : t.probe().p1)
        presses += f.is_pressed(jump) ? 1 : 0;
    CHECK(presses == 3);                                        // ticks 5, 7 and 9 (was 0: neutral frames)
    t.input().set_driver(nullptr);
}
