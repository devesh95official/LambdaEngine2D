// Smaller App features: optional events, reserve_event, find_world, data_dir and freezing worlds.
#include <lam/core/app.h> // first and alone: enough for ctx.commands() (no commands.h include)

#include <lam/test/support.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "app_test_util.h"

using namespace lam; // brings "..."_id too (inline namespace lam::literals)

namespace {

struct Burst { // an optional plugin's World event (BurstAt without ParticlesPlugin)
    int n = 0;
};
struct Chime { // an App event
    int n = 0;
};
struct Unregistered {};

// App resource: what the systems saw.
struct Seen {
    int emitted = 0;
    int skipped = 0;
    int read = 0;
    int chimes = 0;
    bool had_burst = false;
    bool had_unregistered = true;
};

void try_emit_events(Ctx &ctx) {
    Seen &s = ctx.res<Seen>();
    s.had_burst = ctx.has_event<Burst>();
    s.had_unregistered = ctx.has_event<Unregistered>();
    if (ctx.try_emit(Burst{1}))
        ++s.emitted;
    else
        ++s.skipped;
    if (ctx.try_emit(Chime{1})) ++s.chimes;
}
void read_bursts(Ctx &ctx) {
    if (!ctx.has_event<Burst>()) return; // optional: reading an unregistered type aborts
    ctx.res<Seen>().read += static_cast<int>(ctx.read<Burst>().size());
}

void install(App &app) {
    app.insert_resource<Seen>();
    app.add_system(Stage::Simulate, try_emit_events, "t.try_emit");
    app.add_system(Stage::PostPhysics, read_bursts, "t.read_bursts");
}

// World resource of the "play" world.
struct Play {
    int ticks = 0;
    bool over = false;
};
void count_play(Ctx &ctx) {
    ++ctx.local<Play>().ticks;
}
void end_run(Ctx &ctx) {
    if (ctx.world().tick() == 5) ctx.app().quit(0);
}

// A game rule gated by run_if, and an "engine" system on the same world that is not.
struct Motion {
    int game = 0;
    int engine = 0;
};
bool game_running(const Ctx &ctx) {
    return !ctx.local<Motion>().game || ctx.local<Motion>().game < 3;
}
void game_rule(Ctx &ctx) {
    ++ctx.local<Motion>().game;
}
void engine_motor(Ctx &ctx) {
    if (Motion *m = ctx.try_local<Motion>()) ++m->engine;
}

// ctx.commands() compiles and works with <lam/core/app.h> alone.
void despawn_all_tagged(Ctx &ctx) {
    World &w = ctx.world();
    for (auto [e, play] : w.view<Play>()) {
        (void)play;
        ctx.commands().destroy(e);
    }
}

std::string env_or_empty(const char *name) {
    const char *v = std::getenv(name);
    return v ? v : "";
}

} // namespace

TEST_CASE("has_event and try_emit: optional events never abort; registered ones are delivered") {
    {
        test::HeadlessApp h;
        App &app = h.app();
        install(app);
        CHECK_FALSE(app.has_event<Burst>());
        h.ticks(3);
        const Seen &s = app.res<Seen>();
        CHECK(s.skipped == 3);
        CHECK(s.emitted == 0);
        CHECK(s.read == 0);
        CHECK(s.chimes == 0);
        CHECK_FALSE(s.had_burst);
    }
    {
        test::HeadlessApp h;
        App &app = h.app();
        install(app);
        app.add_event<Burst>();
        app.add_event<Chime>(EventScope::App);
        CHECK(app.has_event<Burst>());
        CHECK(app.has_event<Chime>());
        CHECK_FALSE(app.has_event<Unregistered>());
        h.ticks(3);
        const Seen &s = app.res<Seen>();
        CHECK(s.emitted == 3);
        CHECK(s.skipped == 0);
        CHECK(s.read == 3); // PostPhysics reads Simulate's events in the same tick
        CHECK(s.chimes == 3);
        CHECK(s.had_burst);
        CHECK_FALSE(s.had_unregistered);
        CHECK(app.events().get<Chime>().all().size() >= 1);
    }
}

TEST_CASE("reserve_event raises a channel on every world, later worlds and the App bus") {
    test::HeadlessApp h;
    App &app = h.app();
    const std::size_t k = static_cast<std::size_t>(app.config().max_ticks_per_frame + 1);
    app.add_event<Burst>(EventScope::World, 4);
    CHECK(app.main_world().events().get<Burst>().capacity() == 4 * k);
    World &a = app.create_world("a");
    CHECK(a.events().get<Burst>().capacity() == 4 * k);
    app.reserve_event<Burst>(100);
    CHECK(app.main_world().events().get<Burst>().capacity() == 100 * k);
    CHECK(a.events().get<Burst>().capacity() == 100 * k);
    World &b = app.create_world("b"); // created after the reserve
    CHECK(b.events().get<Burst>().capacity() == 100 * k);
    app.reserve_event<Burst>(2); // never shrinks
    CHECK(a.events().get<Burst>().capacity() == 100 * k);
    app.add_event<Burst>(EventScope::World, 500); // a later add_event keeps the first capacity
    CHECK(app.create_world("c").events().get<Burst>().capacity() == 100 * k);
    app.add_event<Chime>(EventScope::App, 4);
    app.reserve_event<Chime>(50);
    CHECK(app.events().get<Chime>().capacity() == 50 * k);
    // Unregistered: LAM_ASSERT (Release ignores the call).
    CHECK(apptest::dies([] {
              test::HeadlessApp bad;
              bad.app().reserve_event<Unregistered>(10);
          }) == static_cast<bool>(LAM_ENABLE_ASSERTS));
}

TEST_CASE("find_world: by name, first created wins, queued destroys and unknown names are null") {
    test::HeadlessApp h;
    App &app = h.app();
    CHECK(app.find_world("main"_id) == &app.main_world());
    World &first = app.create_world("dup");
    (void)app.create_world("dup");
    CHECK(app.find_world("dup"_id) == &first);
    CHECK(app.find_world("nope"_id) == nullptr);
    World &gone = app.create_world("gone");
    struct Doomed {
        World *world = nullptr;
        bool found_after_queue = true;
    };
    app.insert_resource<Doomed>(Doomed{&gone});
    SystemDesc d;
    d.stage = Stage::Simulate;
    d.name = "t.destroy_gone";
    d.only_world = app.main_world().id();
    d.fn = [](Ctx &ctx) {
        Doomed &dm = ctx.res<Doomed>();
        if (!dm.world) return;
        ctx.app().destroy_world(*dm.world); // queued: a pass is running
        dm.world = nullptr;
        dm.found_after_queue = ctx.app().find_world("gone"_id) != nullptr;
    };
    app.add_system(d);
    h.ticks(1);
    CHECK_FALSE(app.res<Doomed>().found_after_queue);
    CHECK(app.find_world("gone"_id) == nullptr); // destroyed at the end of the pass
}

TEST_CASE("after run() returns every world, resource and stack entry is still readable") {
    test::HeadlessApp h;
    App &app = h.app();
    World &play = app.create_world("play");
    play.resources().insert<Play>();
    const WorldEntry stack[] = {{&app.main_world(), true, true, false}, {&play, true, true, true}};
    app.set_world_stack(stack);
    SystemDesc count;
    count.stage = Stage::Simulate;
    count.fn = count_play;
    count.name = "t.count_play";
    count.run_if = &has_local<Play>; // the usual pattern: only worlds holding Play
    app.add_system(count);
    app.add_system(Stage::TickEnd, end_run, "t.end_run");
    app.insert_resource<Seen>();
    CHECK(app.run() == 0);
    // The headless summary pattern.
    World *found = app.find_world("play"_id);
    REQUIRE(found == &play);
    CHECK(found->resources().get<Play>().ticks == 5);
    CHECK(found->tick() == 5);
    CHECK(found->state_hash() == play.state_hash());
    CHECK(app.try_res<Seen>() != nullptr);
    REQUIRE(app.world_stack().size() == 2);
    CHECK(app.world_stack()[1].world == &play);
    CHECK(app.focused_world() == &play);
}

TEST_CASE("Ctx::commands works with <lam/core/app.h> alone") {
    test::HeadlessApp h;
    App &app = h.app();
    World &w = app.main_world();
    const Entity e = w.create();
    w.add<Play>(e);
    app.add_system(Stage::Simulate, despawn_all_tagged, "t.despawn");
    h.ticks(1);
    CHECK_FALSE(w.alive(e));
}

TEST_CASE("run_if skips one system only; ticks = false or set_paused freeze the whole world") {
    test::HeadlessApp h;
    App &app = h.app();
    World &play = app.create_world("play");
    play.resources().insert<Motion>();
    const WorldEntry ticking[] = {{&app.main_world(), true, true, false}, {&play, true, true, true}};
    app.set_world_stack(ticking);
    SystemDesc rule;
    rule.stage = Stage::Simulate;
    rule.fn = game_rule;
    rule.name = "t.game_rule";
    rule.run_if = [](const Ctx &ctx) { return has_local<Motion>(ctx) && game_running(ctx); };
    app.add_system(rule);
    SystemDesc motor;
    motor.stage = Stage::Physics;
    motor.fn = engine_motor;
    motor.name = "t.engine_motor";
    motor.order = -1500;
    app.add_system(motor);
    h.ticks(10);
    const Motion &m = play.resources().get<Motion>();
    CHECK(m.game == 3);    // "game over" after 3 ticks: the gated rule stops...
    CHECK(m.engine == 10); // ...but engine systems on the same world keep running
    // Freeze the world: its tick stages stop (WorldEntry::ticks = false).
    const std::uint64_t play_tick = play.tick();
    const WorldEntry frozen[] = {{&app.main_world(), true, true, false},
                                 {&play, false, true, true}};
    app.set_world_stack(frozen);
    h.ticks(4);
    CHECK(m.engine == 10);
    CHECK(play.tick() == play_tick);
    // Or hold every tick of the App.
    app.set_world_stack(ticking);
    app.set_paused(true);
    const std::uint64_t main_tick = app.main_world().tick();
    h.frames(4);
    CHECK(m.engine == 10);
    CHECK(app.main_world().tick() == main_tick);
    app.set_paused(false);
    h.ticks(1);
    CHECK(m.engine == 11);
}

TEST_CASE("data_dir: LAM_DATA_DIR when set; empty under --headless otherwise") {
    const std::string saved = env_or_empty("LAM_DATA_DIR");
    test::TempDir dir;
    const std::string sub = dir.file("saves");
    ::setenv("LAM_DATA_DIR", sub.c_str(), 1);
    {
        test::HeadlessApp h;
        CHECK(h.app().data_dir() == sub); // tests and bots opt in explicitly
        CHECK(std::filesystem::is_directory(sub)); // created on call
    }
    {
        apptest::ManualApp m; // not headless
        CHECK(m.app.data_dir() == sub);
    }
    {
        const char *const argv[] = {"game", "--bot", "--frames", "60"};
        App windowed_bot(AppConfig{}, 4, argv);
        CHECK(windowed_bot.data_dir() == sub); // scripted, but LAM_DATA_DIR opts in
    }
    ::unsetenv("LAM_DATA_DIR");
    {
        test::HeadlessApp h;
        CHECK(h.app().data_dir().empty()); // never the player's folder in a headless run
    }
    for (const char *flag : {"--bot", "--hidden", "--step"}) {
        const char *const argv[] = {"game", flag};
        App scripted(AppConfig{}, 2, argv); // Nor in a scripted windowed one
        CHECK(scripted.data_dir().empty());
    }
    if (!saved.empty())
        ::setenv("LAM_DATA_DIR", saved.c_str(), 1);
    else
        ::unsetenv("LAM_DATA_DIR");
}

TEST_CASE("AppConfig designated initialisers in declaration order") {
    const AppConfig c{.title = "Order",
                      .window_size = {640, 360},
                      .virtual_size = {320, 180},
                      .tick_rate = 30,
                      .max_frame_seconds = 0.25,
                      .max_ticks_per_frame = 3,
                      .seed = 7,
                      .resizable = false,
                      .vsync = false,
                      .audio = false,
                      .wait_for_startup_assets = false,
                      .entity_capacity = 128,
                      .settings_file = ""};
    CHECK(c.title == "Order");
    CHECK(c.max_ticks_per_frame == 3);
    CHECK(c.settings_file.empty());
}

namespace {
struct SameStage {
    std::vector<std::uint64_t> emitted_at, read_at;
};
void emit_early(Ctx &ctx) {
    ctx.res<SameStage>().emitted_at.push_back(ctx.world().tick());
    ctx.emit(Burst{1});
}
void read_late(Ctx &ctx) {
    for (const Burst &b : ctx.read<Burst>()) {
        (void)b;
        ctx.res<SameStage>().read_at.push_back(ctx.world().tick());
    }
}
void noop(Ctx &) {}
} // namespace

TEST_CASE("an event emitted earlier in the same stage pass is read on the next tick") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<SameStage>();
    app.add_event<Burst>();
    app.add_system(Stage::Simulate, emit_early, "t.emit_early", -10);
    app.add_system(Stage::Simulate, read_late, "t.read_late", 10); // later in the same pass
    h.ticks(3);
    const SameStage &s = app.res<SameStage>();
    REQUIRE(s.emitted_at.size() == 3);
    REQUIRE(s.read_at.size() == 2); // the third tick's event is still pending
    CHECK(s.read_at[0] == s.emitted_at[0] + 1);
    CHECK(s.read_at[1] == s.emitted_at[1] + 1);
}

TEST_CASE("system names are unique per (stage, only_world), so another world may reuse them") {
    test::HeadlessApp h;
    App &app = h.app();
    World &a = app.create_world("a");
    World &b = app.create_world("b");
    SystemDesc d;
    d.stage = Stage::Simulate;
    d.fn = noop;
    d.name = "scene.rules";
    d.only_world = a.id();
    app.add_system(d);
    d.only_world = b.id(); // a re-created scene world registers the same names
    app.add_system(d);
    d.stage = Stage::Update;
    app.add_system(d); // another stage
    h.ticks(1);
    CHECK(app.schedule_text().find("scene.rules") != std::string::npos);
    // The same (stage, only_world, name) twice: LAM_ASSERT (Release keeps both).
    CHECK(apptest::dies([] {
              test::HeadlessApp dup;
              dup.app().add_system(Stage::Simulate, noop, "t.twice");
              dup.app().add_system(Stage::Simulate, noop, "t.twice");
          }) == static_cast<bool>(LAM_ENABLE_ASSERTS));
}
