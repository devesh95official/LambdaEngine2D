// Event windows: each reader sees every event once, in order, at most one tick late, at any
// frame pacing.
#include <lam/test/support.h>

#include <algorithm>
#include <map>

#include "app_test_util.h"

using namespace lam;

namespace {

struct Ping {
    std::uint32_t id = 0;
};
struct AppPing {
    std::uint32_t id = 0;
};

// App resource: emissions and reads, by id.
struct Book {
    bool emit = true;
    std::uint32_t next = 1;
    std::vector<std::uint32_t> emitted;                  // emission order
    std::map<std::uint32_t, std::uint64_t> emitted_tick; // world tick at emission
    std::map<std::uint32_t, bool> from_tick;             // emitted by a tick stage
    std::vector<std::uint32_t> sim_reads, update_reads, frame_end_reads, app_reads_sim;
    std::vector<std::uint64_t> sim_lateness; // read tick - emission tick
};

void emit_ping(Ctx &ctx, bool from_tick) {
    Book &b = ctx.res<Book>();
    if (!b.emit) return;
    const std::uint32_t id = b.next++;
    b.emitted.push_back(id);
    b.emitted_tick[id] = ctx.world().tick();
    b.from_tick[id] = from_tick;
    ctx.emit(Ping{id});
}
void animate_emit(Ctx &ctx) {
    emit_ping(ctx, true);
}
void update_emit(Ctx &ctx) {
    emit_ping(ctx, false);
}
void simulate_read(Ctx &ctx) {
    Book &b = ctx.res<Book>();
    for (const Ping &p : ctx.read<Ping>()) {
        b.sim_reads.push_back(p.id);
        b.sim_lateness.push_back(ctx.world().tick() - b.emitted_tick[p.id]);
    }
}
void update_read(Ctx &ctx) {
    for (const Ping &p : ctx.read<Ping>())
        ctx.res<Book>().update_reads.push_back(p.id);
}
void frame_end_read(Ctx &ctx) {
    for (const Ping &p : ctx.read<Ping>())
        ctx.res<Book>().frame_end_reads.push_back(p.id);
}

SystemDesc desc(Stage s, SystemFn fn, std::string_view name, int order, SystemScope scope) {
    SystemDesc d;
    d.stage = s;
    d.fn = fn;
    d.name = name;
    d.order = order;
    d.scope = scope;
    return d;
}

void install(App &app) {
    app.insert_resource<Book>();
    app.add_event<Ping>();
    app.add_system(Stage::Animate, animate_emit, "t.animate_emit");
    app.add_system(desc(Stage::Update, update_read, "t.update_read", -10, SystemScope::PerWorld));
    app.add_system(Stage::Update, update_emit, "t.update_emit", 0);
    app.add_system(Stage::Simulate, simulate_read, "t.simulate_read");
    app.add_system(desc(Stage::FrameEnd, frame_end_read, "t.frame_end_read", 0, SystemScope::Once));
}

// Two frames with one tick each deliver everything still in flight.
void drain(apptest::ManualApp &m) {
    m.app.res<Book>().emit = false;
    m.frame_after(1);
    m.frame_after(1);
}

} // namespace

TEST_CASE("events: exactly once and in order with 0, 1 and 4 ticks per frame") {
    apptest::ManualApp m;
    install(m.app);
    // Tick lengths per frame: 0-tick frames, 1-tick frames, 4-tick frames, a hitch (capped at 4),
    // and half ticks that alternate 0 and 1.
    const double pacing[] = {0, 1, 1, 0, 0, 4, 1, 0.5, 0.5, 0.5, 0.5, 10, 0, 2, 1, 4, 0, 0, 3, 1};
    std::vector<int> ticks_seen;
    for (double p : pacing) {
        const std::uint64_t before = m.app.tick();
        m.frame_after(p);
        ticks_seen.push_back(static_cast<int>(m.app.tick() - before));
    }
    CHECK(std::find(ticks_seen.begin(), ticks_seen.end(), 0) != ticks_seen.end());
    CHECK(std::find(ticks_seen.begin(), ticks_seen.end(), 1) != ticks_seen.end());
    CHECK(std::find(ticks_seen.begin(), ticks_seen.end(), 4) != ticks_seen.end());
    CHECK(*std::max_element(ticks_seen.begin(), ticks_seen.end()) == 4);
    drain(m);
    const Book &b = m.app.res<Book>();
    REQUIRE(b.emitted.size() > 20);
    CHECK(b.sim_reads == b.emitted);
    CHECK(b.update_reads == b.emitted);
    CHECK(b.frame_end_reads == b.emitted);
    // At most one tick late: tick-stage emissions are read by the next tick; frame emissions by
    // the first tick after them.
    for (std::size_t i = 0; i < b.sim_reads.size(); ++i)
        CHECK(b.sim_lateness[i] == 1);
}

TEST_CASE("events: an Animate event at tick N is read by Simulate at tick N+1 in the same frame") {
    apptest::ManualApp m;
    install(m.app);
    m.frame_after(0); // first frame: dt 0, no tick; Update emits 1
    m.frame_after(4);
    const Book &b = m.app.res<Book>();
    REQUIRE(m.app.tick() == 4);
    // Tick 1 reads 1 (frame event); ticks 1-4 emit 2-5 in Animate, read by ticks 2-4 (2-4) and
    // by this frame's Update (2-5); Update then emits 6.
    CHECK(b.sim_reads == std::vector<std::uint32_t>{1, 2, 3, 4});
    CHECK(b.update_reads == std::vector<std::uint32_t>{1, 2, 3, 4, 5});
    for (std::uint64_t late : b.sim_lateness)
        CHECK(late == 1);
}

namespace {
struct StackCtl {
    World *other = nullptr;
};
void set_ticking(App &app, bool other_ticks) {
    World *o = app.res<StackCtl>().other;
    const WorldEntry stack[] = {{&app.main_world(), true, true, true},
                                {o, other_ticks, true, false}};
    app.set_world_stack(stack);
}

struct WorldBook {
    std::map<std::uint32_t, std::vector<std::uint32_t>> reads; // world id -> ids read in Simulate
    std::map<std::uint32_t, std::vector<std::uint32_t>> emits; // world id -> ids emitted in Update
    std::uint32_t next = 1;
};
void per_world_emit(Ctx &ctx) {
    WorldBook &b = ctx.res<WorldBook>();
    const std::uint32_t id = b.next++;
    b.emits[ctx.world().id()].push_back(id);
    ctx.emit(Ping{id});
}
void per_world_read(Ctx &ctx) {
    for (const Ping &p : ctx.read<Ping>())
        ctx.res<WorldBook>().reads[ctx.world().id()].push_back(p.id);
}
} // namespace

TEST_CASE(
    "events: a world that stops ticking skips what is emitted meanwhile, keeps what was pending") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<WorldBook>();
    app.add_event<Ping>();
    app.add_system(Stage::Update, per_world_emit, "t.emit");
    app.add_system(Stage::Simulate, per_world_read, "t.read");
    World &o = app.create_world("other");
    app.insert_resource<StackCtl>(StackCtl{&o});
    set_ticking(app, true);
    h.ticks(3); // o ticks
    set_ticking(app, false);
    h.ticks(3); // o paused: frame events skipped
    set_ticking(app, true);
    h.ticks(3);
    const WorldBook &b = app.res<WorldBook>();
    const auto &oe = b.emits.at(o.id());
    const auto &orr = b.reads.at(o.id());
    REQUIRE(oe.size() == 9);
    // Frames 1-3 emitted oe[0..2] while o ticked: oe[0], oe[1] were read in frames 2-3; oe[2] was
    // pending when o stopped ticking and is delivered on resume. oe[3..5] were emitted while o
    // could not tick: skipped. oe[6], oe[7] are read after the resume; oe[8] is still in flight.
    std::vector<std::uint32_t> expected = {oe[0], oe[1], oe[2], oe[6], oe[7]};
    CHECK(orr == expected);
    // The main world ticked throughout and lost nothing.
    const auto &me = b.emits.at(app.main_world().id());
    const auto &mr = b.reads.at(app.main_world().id());
    CHECK(mr == std::vector<std::uint32_t>(me.begin(), me.end() - 1));
}

namespace {
void emit_app_ping(Ctx &ctx) {
    Book &b = ctx.res<Book>();
    const std::uint32_t id = b.next++;
    b.emitted.push_back(id);
    ctx.emit(AppPing{id});
}
void read_app_ping(Ctx &ctx) {
    for (const AppPing &p : ctx.read<AppPing>()) {
        apptest::trace(ctx, "app" + std::to_string(p.id));
    }
}
} // namespace

TEST_CASE("events: App-scope events go to the App bus and every world reads the same window") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Book>();
    app.insert_resource<apptest::Trace>();
    app.add_event<AppPing>(EventScope::App);
    World &a = app.create_world("a");
    const WorldEntry stack[] = {{&app.main_world(), true, true, true}, {&a, true, true, false}};
    app.set_world_stack(stack);
    app.add_system(desc(Stage::Update, emit_app_ping, "t.emit_app", 0, SystemScope::Once));
    app.add_system(Stage::Simulate, read_app_ping, "t.read_app");
    h.ticks(3);
    CHECK(app.events().get<AppPing>().all().size() >= 1);
    CHECK(a.events().try_get<AppPing>() == nullptr);
    // Frame 1 emits 1 (Update), read by both worlds' Simulate in tick 2; 2 in tick 3.
    CHECK(app.res<apptest::Trace>().joined() == "app1@main app1@a app2@main app2@a");
}

TEST_CASE("events: World-scope events never cross worlds") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<WorldBook>();
    app.add_event<Ping>();
    app.add_system(Stage::Update, per_world_emit, "t.emit");
    app.add_system(Stage::Simulate, per_world_read, "t.read");
    World &a = app.create_world("a");
    const WorldEntry stack[] = {{&app.main_world(), true, true, true}, {&a, true, true, false}};
    app.set_world_stack(stack);
    h.ticks(5);
    const WorldBook &b = app.res<WorldBook>();
    for (const auto &[world, ids] : b.reads) {
        const auto &mine = b.emits.at(world);
        for (std::uint32_t id : ids)
            CHECK(std::find(mine.begin(), mine.end(), id) != mine.end());
    }
    CHECK(b.reads.at(a.id()).size() == 4);
}

TEST_CASE("events: worlds created later get every registered World event; retention is bounded") {
    test::HeadlessApp h;
    App &app = h.app();
    install(app);
    World &late = app.create_world("late");
    CHECK(late.events().try_get<Ping>() != nullptr);
    // Capacity is pre-reserved as capacity x (max_ticks_per_frame + 1).
    CHECK(late.events().get<Ping>().capacity() == 256u * 5u);
    const WorldEntry stack[] = {{&app.main_world(), true, true, true}, {&late, true, true, false}};
    app.set_world_stack(stack);
    h.ticks(300);
    // Each frame emits two events per world; trimming keeps about one frame's worth.
    CHECK(app.main_world().events().get<Ping>().all().size() <= 4);
    CHECK(late.events().get<Ping>().all().size() <= 4);
}

TEST_CASE("events: a non-focused, hidden or stack-less world does not retain events forever") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<WorldBook>();
    app.add_event<Ping>();
    app.add_system(desc(Stage::TickEnd, per_world_emit, "t.emit", 0, SystemScope::PerWorld));
    World &hidden = app.create_world("hidden");
    const WorldEntry stack[] = {{&app.main_world(), true, true, true},
                                {&hidden, true, false, false}};
    app.set_world_stack(stack);
    h.ticks(200);
    CHECK(hidden.events().get<Ping>().all().size() <= 2);
}

TEST_CASE("events: emit or read of an unregistered type aborts with the fix") {
    std::string out;
    CHECK(apptest::dies(
        [] {
            test::HeadlessApp h;
            h.app().add_system(Stage::Simulate, [](Ctx &ctx) { ctx.emit(AppPing{1}); }, "t.bad");
            h.ticks(1);
        },
        &out));
    CHECK(out.find("add_event") != std::string::npos);
}
