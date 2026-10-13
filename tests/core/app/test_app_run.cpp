// App::run: the fixed clock, pause and --step, startup, quitting, determinism and allocation.
#include <lam/test/support.h>

#include <cstdlib>

#include <lam/core/debug.h>
#include <lam/core/fs.h>
#include <lam/core/profiler.h>
#include <lam/core/settings.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/lifetime.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>

#include "app_test_util.h"

using namespace lam;
using namespace lam::literals;
using apptest::Trace;

namespace {

// App resource: Time as seen by Update on each world.
struct Seen {
    std::vector<float> alpha_main, alpha_other, frame_dt_main;
    std::vector<int> ticks_main;
    std::vector<bool> paused;
};
void see_time(Ctx &ctx) {
    Seen &s = ctx.res<Seen>();
    if (&ctx.world() == &ctx.app().main_world()) {
        s.alpha_main.push_back(ctx.time().alpha);
        s.frame_dt_main.push_back(ctx.time().frame_dt);
        s.ticks_main.push_back(ctx.time().ticks_this_frame);
        s.paused.push_back(ctx.time().paused);
    } else {
        s.alpha_other.push_back(ctx.time().alpha);
    }
}

} // namespace

TEST_CASE("run: alpha is the clock remainder for ticking worlds and 1 otherwise") {
    apptest::ManualApp m;
    App &app = m.app;
    app.insert_resource<Seen>();
    World &other = app.create_world("other");
    const WorldEntry stack[] = {{&app.main_world(), true, true, true},
                                {&other, false, true, false}};
    app.set_world_stack(stack);
    app.add_system(Stage::Update, see_time, "t.see");
    m.frame_after(0);   // clock origin
    m.frame_after(1.5); // 1 tick, half a tick banked
    const Seen &s = app.res<Seen>();
    REQUIRE(s.alpha_main.size() == 2);
    CHECK(s.ticks_main[1] == 1);
    CHECK(s.alpha_main[1] == doctest::Approx(0.5));
    CHECK(s.alpha_other[1] == 1.0f);
    CHECK(app.time().alpha == doctest::Approx(0.5));
    m.frame_after(0.25); // 0 ticks, 0.75 banked
    CHECK(s.ticks_main[2] == 0);
    CHECK(s.alpha_main[2] == doctest::Approx(0.75));
    // Paused: no ticks, alpha 1, the clock is reset.
    app.set_paused(true);
    const std::uint64_t tick_before = app.tick();
    const std::uint64_t world_tick_before = app.main_world().tick();
    m.frame_after(3);
    CHECK(app.tick() == tick_before);
    CHECK(app.main_world().tick() == world_tick_before);
    CHECK(s.alpha_main[3] == 1.0f);
    CHECK(s.paused[3]);
    CHECK(app.time().paused);
    app.set_paused(false);
    m.frame_after(0.5); // banked time was dropped
    CHECK(app.tick() == tick_before);
    CHECK(s.alpha_main[4] == doctest::Approx(0.5));
    m.frame_after(0.5);
    CHECK(app.tick() == tick_before + 1);
}

TEST_CASE("run: --step and step_ticks run exactly one tick per frame with alpha 1") {
    SUBCASE("--step") {
        const char *const argv[] = {"game", "--step"};
        AppConfig c;
        c.settings_file = "";
        App app(c, 2, argv);
        auto p = std::make_unique<apptest::ManualPlatform>();
        apptest::ManualPlatform *pp = p.get();
        app.set_platform(std::move(p));
        app.insert_resource<Seen>();
        app.add_system(Stage::Update, see_time, "t.see");
        for (int i = 0; i < 4; ++i) {
            pp->now += i == 2 ? 1.0 : 0.0; // pacing is ignored
            app.run_frames(1);
        }
        CHECK(app.tick() == 4);
        for (float a : app.res<Seen>().alpha_main)
            CHECK(a == 1.0f);
        // Presentation advances one tick per frame too, whatever the wall clock did.
        for (float dt : app.res<Seen>().frame_dt_main)
            CHECK(dt == app.time().tick_dt);
        CHECK(app.res<Seen>().frame_dt_main.size() == 4);
        CHECK(pp->begins == 4);
        CHECK(pp->ends == 4);
    }
    SUBCASE("step_ticks") {
        apptest::ManualApp m;
        m.app.step_ticks(5);
        CHECK(m.app.tick() == 5);
        CHECK(m.app.main_world().tick() == 5);
        CHECK(m.app.time().frame == 4);
    }
    SUBCASE("step_ticks while paused holds ticks") {
        test::HeadlessApp h;
        h.app().set_paused(true);
        CHECK(h.app().paused());
        h.ticks(3);
        CHECK(h.app().tick() == 0);
        h.app().set_paused(false);
        h.ticks(3);
        CHECK(h.app().tick() == 3);
    }
}

TEST_CASE("run: NullPlatform advances exactly 1/tick_rate per frame") {
    NullPlatform p(50, {320, 200});
    CHECK(p.now_seconds() == 0.0);
    p.begin_frame();
    p.begin_frame();
    CHECK(p.now_seconds() == doctest::Approx(2.0 / 50));
    CHECK_FALSE(p.should_close());
    CHECK_FALSE(p.has_gpu());
    CHECK(p.window().framebuffer == IVec2{320, 200});
    CHECK(p.window().safe_area.w == 320);
    p.set_fullscreen(true);
    CHECK(p.window().fullscreen);
    test::HeadlessApp h;
    h.frames(10);
    CHECK(h.app().tick() == 10); // headless: one tick per frame
    CHECK(h.app().time().real_seconds == doctest::Approx(9.0 / 60));
}

namespace {
struct StartupLog {
    std::vector<std::string> lines;
    std::uint32_t rolled = 0;
};
void startup_a(Ctx &ctx) {
    StartupLog &l = ctx.res<StartupLog>();
    l.lines.push_back("a@" + apptest::world_label(ctx.world()) + ":" +
                      std::to_string(ctx.app().tick()));
    l.rolled = ctx.rng("spawn"_id).next_u32(); // allowed in startup
    Entity e = ctx.commands().spawn();
    ctx.commands().add(e, Transform{{3, 4}, 0, {1, 1}});
}
void startup_b(Ctx &ctx) {
    ctx.res<StartupLog>().lines.push_back("b:" + std::to_string(ctx.world().entity_count()));
}
void startup_late(Ctx &ctx) {
    ctx.res<StartupLog>().lines.push_back("late");
}
void first_tick(Ctx &ctx) {
    StartupLog &l = ctx.res<StartupLog>();
    if (l.lines.size() < 8) l.lines.push_back("tick" + std::to_string(ctx.time().tick));
}
} // namespace

TEST_CASE("run: startup systems run once, in order, on the main world before frame 1") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<StartupLog>();
    World &other = app.create_world("other");
    const WorldEntry stack[] = {{&other, true, true, true}};
    app.set_world_stack(stack);
    app.add_startup(startup_a, "t.start_a");
    app.add_startup(startup_b, "t.start_b");
    app.add_system(Stage::Simulate, first_tick, "t.first_tick");
    h.ticks(2);
    StartupLog &l = app.res<StartupLog>();
    // Commands of a are applied before b runs.
    CHECK(l.lines == std::vector<std::string>{"a@main:0", "b:1", "tick1", "tick2"});
    app.add_startup(startup_late, "t.start_late");
    h.ticks(1);
    CHECK(l.lines.back() == "tick3");
    CHECK(l.lines[l.lines.size() - 2] == "late");
}

namespace {
struct FakeResolver final : AssetResolver {
    std::size_t waiting = 0;
    std::uint64_t resolve(TypeId, std::string_view) override { return 0; }
    std::string_view path_of(TypeId, std::uint64_t) const override { return {}; }
    Result<std::string> read_text(std::string_view) const override {
        return make_error(ErrorCode::NotFound, "fake");
    }
    std::size_t pending() const override { return waiting; }
};
} // namespace

TEST_CASE("run: ticks are held until the startup assets have all loaded") {
    test::HeadlessApp h;
    App &app = h.app();
    FakeResolver r;
    r.waiting = 2;
    app.set_asset_resolver(&r);
    CHECK(app.asset_resolver() == &r);
    CHECK(app.serialize_context().assets == &r);
    h.frames(3);
    CHECK(app.tick() == 0);
    CHECK(app.time().frame == 2); // frame stages still run
    r.waiting = 0;
    h.frames(2);
    CHECK(app.tick() == 2);
    r.waiting = 5; // later loads never hold ticks
    h.frames(2);
    CHECK(app.tick() == 4);
    app.set_asset_resolver(nullptr);
}

TEST_CASE("run: wait_for_startup_assets = false never holds") {
    AppConfig c;
    c.wait_for_startup_assets = false;
    test::HeadlessApp h(c);
    FakeResolver r;
    r.waiting = 1;
    h.app().set_asset_resolver(&r);
    h.frames(2);
    CHECK(h.app().tick() == 2);
    h.app().set_asset_resolver(nullptr);
}

namespace {
void quit_at_5(Ctx &ctx) {
    if (ctx.app().tick() == 5) ctx.app().quit(3);
}
} // namespace

TEST_CASE("run: run() loops until quit and returns the exit code; --frames N quits after N") {
    {
        test::HeadlessApp h;
        h.app().add_system(Stage::Simulate, quit_at_5, "t.quit");
        CHECK(h.app().run() == 3);
        CHECK(h.app().quitting());
        CHECK(h.app().tick() == 5);
    }
    {
        const char *const argv[] = {"game", "--headless", "--frames", "7"};
        App app({}, 4, argv);
        CHECK(app.run() == 0);
        CHECK(app.tick() == 7);
    }
    {
        AppConfig c;
        c.settings_file = "";
        App app(c);
        auto p = std::make_unique<apptest::ManualPlatform>();
        p->close = true;
        app.set_platform(std::move(p));
        CHECK(app.run() == 0);
        CHECK(app.quitting());
    }
}

TEST_CASE("run: the constructor inserts the App resources") {
    test::HeadlessApp h;
    App &app = h.app();
    CHECK(app.try_res<SettingsStore>() != nullptr);
    CHECK(app.try_res<PrefabLibrary>() != nullptr);
    CHECK(app.try_res<DebugOverlay>() != nullptr);
    REQUIRE(app.try_res<Profiler>() != nullptr);
    CHECK(detail::current_profiler() == &app.res<Profiler>());
    CHECK(app.resources().has<SettingsStore>());
    CHECK(app.config().title == "LambdaEngine2D");
    CHECK(dynamic_cast<NullPlatform *>(&app.platform()) != nullptr);
    h.ticks(2);
    if constexpr (LAM_DEV || LAM_PROFILE) {
        bool saw_stage = false;
        for (const StageTiming &t : app.res<Profiler>().timings())
            saw_stage = saw_stage || t.name == "Simulate";
        CHECK(saw_stage);
    }
}

TEST_CASE(
    "run: settings load from LAM_DATA_DIR/<title>, save on quit; headless never touches them") {
    test::TempDir dir;
    const char *old = std::getenv("LAM_DATA_DIR");
    const std::string saved_env = old ? old : "";
    ::setenv("LAM_DATA_DIR", dir.path().c_str(), 1);
    const std::string file = join_path(dir.path(), "settings.json");
    {
        AppConfig c;
        c.title = "SettingsTest";
        App app(c);
        SettingsStore &s = app.res<SettingsStore>();
        s.define_int("volume", 5, 0, 10);
        s.set_int("volume", 8);
        app.quit(0);
    }
    CHECK(file_exists(file));
    {
        AppConfig c;
        c.title = "SettingsTest";
        App app(c);
        SettingsStore &s = app.res<SettingsStore>();
        s.define_int("volume", 5, 0, 10);
        CHECK(s.get_int("volume") == 8);
    }
    REQUIRE(remove_file(file).ok());
    {
        // LifecycleChanged::Background (App event, emitted by the platform) saves at FrameBegin.
        AppConfig c;
        c.title = "SettingsTest";
        App app(c);
        app.res<SettingsStore>().define_int("volume", 5, 0, 10);
        app.run_frames(1);
        CHECK_FALSE(file_exists(file));
        app.events().get<LifecycleChanged>().emit(LifecycleChanged{LifecycleEvent::Background});
        app.run_frames(1);
        CHECK(file_exists(file));
    }
    REQUIRE(remove_file(file).ok());
    {
        AppConfig c;
        c.title = "SettingsTest";
        test::HeadlessApp h(c);
        h.app().res<SettingsStore>().define_int("volume", 5, 0, 10);
        h.app().res<SettingsStore>().set_int("volume", 9);
        h.app().quit(0);
    }
    CHECK_FALSE(file_exists(file));
    ::setenv("LAM_DATA_DIR", saved_env.c_str(), 1);
}

TEST_CASE("run: scripted windowed runs neither load nor save settings") {
    test::TempDir dir;
    const char *old = std::getenv("LAM_DATA_DIR");
    const std::string saved_env = old ? old : "";
    ::setenv("LAM_DATA_DIR", dir.path().c_str(), 1);
    const std::string file = join_path(dir.path(), "settings.json");
    {
        AppConfig c; // a player run writes the file the scripted runs below must ignore
        c.title = "SettingsTest";
        App app(c);
        app.res<SettingsStore>().define_int("volume", 5, 0, 10);
        app.res<SettingsStore>().set_int("volume", 8);
        app.quit(0);
    }
    REQUIRE(file_exists(file));
    const Result<std::string> before = read_file(file);
    REQUIRE(before.ok());
    const char *const frames[] = {"game", "--frames", "3"};
    const char *const hidden[] = {"game", "--hidden"};
    const char *const bot[] = {"game", "--bot"};
    const char *const step[] = {"game", "--step"};
    for (const auto &argv : {frames, hidden, bot, step}) {
        const int argc = argv == frames ? 3 : 2;
        AppConfig c;
        c.title = "SettingsTest";
        App app(c, argc, argv);
        REQUIRE(app.options().scripted());
        SettingsStore &s = app.res<SettingsStore>();
        s.define_int("volume", 5, 0, 10);
        CHECK(s.get_int("volume") == 5); // not loaded
        s.set_int("volume", 2);
        app.events().get<LifecycleChanged>().emit(LifecycleChanged{LifecycleEvent::Background});
        app.run_frames(1);
        app.quit(0);
    }
    const Result<std::string> after = read_file(file);
    REQUIRE(after.ok());
    CHECK(*after == *before); // never saved
    ::setenv("LAM_DATA_DIR", saved_env.c_str(), 1);
}

namespace {
void overlay_stat(Ctx &ctx) {
    ctx.res<DebugOverlay>().stat("t.stat", 1.0);
}
} // namespace

TEST_CASE("run: DebugOverlay is cleared and the Profiler published once per frame") {
    test::HeadlessApp h;
    App &app = h.app();
    app.add_system(Stage::Update, overlay_stat, "t.overlay");
    h.frames(1);
    CHECK(app.res<DebugOverlay>().stats().empty());
    if constexpr (LAM_DEV || LAM_PROFILE) CHECK_FALSE(app.res<Profiler>().timings().empty());
}

namespace {
struct Spot {};
void needs_world_res_as_app(Ctx &ctx) {
    (void)ctx.res<DebugDraw>();
}
void needs_app_res_as_world(Ctx &ctx) {
    (void)ctx.local<Profiler>();
}
} // namespace

TEST_CASE("run: Ctx::res/local name the right call when used on the wrong scope") {
    test::HeadlessApp h;
    App &app = h.app();
    CHECK(app.main_world().resources().has<DebugDraw>());
    std::string out;
    CHECK(apptest::dies(
        [] {
            test::HeadlessApp a;
            a.app().add_system(Stage::Simulate, needs_world_res_as_app, "t.bad");
            a.ticks(1);
        },
        &out));
    CHECK(out.find("ctx.local<T>()") != std::string::npos);
    CHECK(apptest::dies(
        [] {
            test::HeadlessApp a;
            a.app().add_system(Stage::Simulate, needs_app_res_as_world, "t.bad");
            a.ticks(1);
        },
        &out));
    CHECK(out.find("ctx.res<T>()") != std::string::npos);
    CHECK(apptest::dies(
        [] {
            test::HeadlessApp a;
            a.app().add_system(Stage::Simulate, [](Ctx &ctx) { (void)ctx.res<Spot>(); }, "t.bad");
            a.ticks(1);
        },
        &out));
    CHECK(out.find("insert_resource") != std::string::npos);
}

TEST_CASE("run: Ctx::rng is for tick stages; fx_rng is App-level") {
    CHECK(apptest::dies([] {
              test::HeadlessApp a;
              a.app().add_system(Stage::Update, [](Ctx &ctx) { (void)ctx.rng("x"_id); }, "t.bad");
              a.ticks(1);
          }) == static_cast<bool>(LAM_ENABLE_ASSERTS));
    test::HeadlessApp h;
    CHECK(&h.app().fx_rng() == &h.app().fx_rng());
}

// ---- Determinism and allocation -----------------------------------------------------------------

namespace app_run_test {
struct Mover {
    lam::Vec2 v;
};
} // namespace app_run_test
LAM_REFLECT(app_run_test::Mover, v)
using app_run_test::Mover;

namespace {
void spawn_movers(Ctx &ctx) {
    World &w = ctx.world();
    Rng &rng = ctx.rng("spawn"_id);
    for (int i = 0; i < 64; ++i) {
        Entity e = w.create();
        w.add<Transform>(e, Transform{{rng.range(0, 480), rng.range(0, 270)}, 0, {1, 1}});
        w.add<Mover>(e, Mover{{rng.range(-2, 2), rng.range(-2, 2)}});
        if (i % 8 == 0)
            w.add<Lifetime>(e, Lifetime{static_cast<std::uint32_t>(rng.between(5, 50))});
    }
}
void move_movers(Ctx &ctx) {
    Rng &rng = ctx.rng("jitter"_id);
    ctx.world().view<Transform, Mover>().each([&](Entity, Transform &t, Mover &m) {
        t.position = t.position + m.v;
        if (rng.chance(0.05f)) m.v = Vec2{rng.range(-2, 2), rng.range(-2, 2)};
    });
}
// Keeps the population stable: one spawn per tick through Commands (exercises JustSpawned).
void respawn(Ctx &ctx) {
    Rng &rng = ctx.rng("respawn"_id);
    Commands &c = ctx.commands();
    Entity e = c.spawn();
    c.add(e, Transform{{rng.range(0, 480), 0}, 0, {1, 1}});
    c.add(e, Mover{{0, rng.range(0, 1)}});
    c.add(e, Lifetime{static_cast<std::uint32_t>(rng.between(10, 30))});
}
void read_restored(Ctx &ctx) {
    (void)ctx.read<WorldRestored>();
}

void install_sim(App &app) {
    app.register_component<Mover>();
    app.add_startup(spawn_movers, "t.spawn");
    app.add_system(Stage::Simulate, move_movers, "t.move");
    app.add_system(Stage::Simulate, respawn, "t.respawn", 10);
    app.add_system(Stage::Update, read_restored, "t.read_restored");
}
} // namespace

TEST_CASE("run: same seed, same state hash across two headless Apps; another seed differs") {
    auto run = [](std::uint64_t seed) {
        AppConfig c;
        c.seed = seed;
        test::HeadlessApp h(c);
        install_sim(h.app());
        h.ticks(200);
        return std::pair{h.app().main_world().state_hash(), h.app().main_world().entity_count()};
    };
    const auto a = run(7);
    const auto b = run(7);
    const auto c = run(8);
    CHECK(a.first == b.first);
    CHECK(a.second == b.second);
    CHECK(a.first != c.first);
}

TEST_CASE("run: --hash-every logs the state hash of ticking worlds") {
    test::CaptureLog log;
    const char *const argv[] = {"game", "--headless", "--hash-every", "2"};
    App app({}, 4, argv);
    app.run_frames(4);
    CHECK(log.contains("hash tick=2 world=main"));
    CHECK(log.contains("hash tick=4 world=main"));
    CHECK_FALSE(log.contains("hash tick=3"));
}

TEST_CASE("run: align_pools keeps PrevTransform in Transform's dense order") {
    test::HeadlessApp h;
    App &app = h.app();
    install_sim(app);
    h.ticks(30);
    std::vector<std::uint32_t> lead, follow;
    app.main_world().view<Transform>().each(
        [&](Entity e, Transform &) { lead.push_back(e.index); });
    app.main_world().view<PrevTransform>().each(
        [&](Entity e, PrevTransform &) { follow.push_back(e.index); });
    REQUIRE(!lead.empty());
    CHECK(follow == lead);
}

TEST_CASE("run: no allocation per frame in steady state (two worlds, events, commands)") {
    test::HeadlessApp h;
    App &app = h.app();
    install_sim(app);
    World &ui = app.create_world("ui");
    const WorldEntry stack[] = {{&app.main_world(), true, true, false}, {&ui, false, true, true}};
    app.set_world_stack(stack);
    test::run_ticks(app, 120);
    CHECK_NO_ALLOC(test::run_ticks(app, 600));
    CHECK_NO_ALLOC(test::run_frames(app, 60));
}

TEST_CASE("run: CHECK_NO_ALLOC catches an allocating frame (canary)") {
    test::HeadlessApp h;
    App &app = h.app();
    app.add_system(
        Stage::Update, [](Ctx &ctx) { ctx.res<Trace>().add(std::string(64, 'x')); }, "t.alloc");
    app.insert_resource<Trace>();
    test::run_ticks(app, 2);
    if (allocation_counting_enabled()) MUST_FAIL(CHECK_NO_ALLOC(test::run_ticks(app, 3)));
}
