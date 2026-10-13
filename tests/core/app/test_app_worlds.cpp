// Worlds and the world stack: PerWorld vs Once, stack flags, focus, seeds and destruction.
#include <lam/test/support.h>

#include <lam/core/debug.h>
#include <lam/ecs/prefab.h>

#include "app_test_util.h"

using namespace lam;
using apptest::Trace;

namespace {

SystemDesc desc(Stage s, SystemFn fn, std::string_view name, int order, SystemScope scope) {
    SystemDesc d;
    d.stage = s;
    d.fn = fn;
    d.name = name;
    d.order = order;
    d.scope = scope;
    return d;
}

void tick_world(Ctx &ctx) {
    apptest::trace(ctx, "sim");
}
void update_world(Ctx &ctx) {
    apptest::trace(ctx, "up");
}
void tick_once(Ctx &ctx) {
    apptest::trace(ctx, ctx.focused() ? "tb.once*" : "tb.once");
}
void frame_once(Ctx &ctx) {
    apptest::trace(ctx, ctx.focused() ? "fe.once*" : "fe.once");
}
void focus_world(Ctx &ctx) {
    if (ctx.focused()) apptest::trace(ctx, "focused");
}

// App resource: a stack to install from inside a system.
struct NextStack {
    std::vector<WorldEntry> entries;
    bool done = false;
};
void install_stack(Ctx &ctx) {
    NextStack &n = ctx.res<NextStack>();
    if (n.done) return;
    n.done = true;
    ctx.app().set_world_stack(n.entries);
}

struct Doomed {
    World *world = nullptr;
};
void destroy_in_system(Ctx &ctx) {
    Doomed &d = ctx.res<Doomed>();
    if (!d.world) return;
    apptest::trace(ctx, "destroy");
    ctx.app().destroy_world(*d.world);
    d.world = nullptr;
}

} // namespace

TEST_CASE(
    "worlds: ticks/visible/input flags select PerWorld runs; Once runs on the focused world") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    World &a = app.create_world("a");
    World &b = app.create_world("b");
    // main: ticks, visible, no input. a: not ticking, visible, input. b: ticks, hidden, input but
    // below a, so a is focused (topmost input world wins).
    const WorldEntry stack[] = {
        {&app.main_world(), true, true, false}, {&b, true, false, true}, {&a, false, true, true}};
    app.set_world_stack(stack);
    app.add_system(Stage::Simulate, tick_world, "t.sim");
    app.add_system(Stage::Update, update_world, "t.up");
    app.add_system(desc(Stage::TickBegin, tick_once, "t.tb_once", 0, SystemScope::Once));
    app.add_system(desc(Stage::FrameEnd, frame_once, "t.fe_once", 0, SystemScope::Once));
    h.ticks(1);
    // The stack is read after the first FrameBegin Once pass, so the TickBegin Once pass already
    // sees it.
    CHECK(app.res<Trace>().joined() == "tb.once*@a sim@main sim@b up@main up@a fe.once*@a");
    CHECK(app.focused_world() == &a);
    CHECK(app.world_stack().size() == 3);
    CHECK(app.main_world().tick() == 1);
    CHECK(b.tick() == 1);
    CHECK(a.tick() == 0);
    CHECK(app.tick() == 1);
}

TEST_CASE("worlds: without an input world, Once systems use the main world and nobody is focused") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    World &a = app.create_world("a");
    const WorldEntry stack[] = {{&a, true, true, false}};
    app.set_world_stack(stack);
    app.add_system(desc(Stage::FrameEnd, frame_once, "t.fe_once", 0, SystemScope::Once));
    app.add_system(Stage::Simulate, focus_world, "t.focus");
    h.ticks(1);
    CHECK(app.focused_world() == nullptr);
    CHECK(app.res<Trace>().joined() == "fe.once@main");
}

TEST_CASE("worlds: an empty stack means {main_world}") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    app.add_system(Stage::Simulate, tick_world, "t.sim");
    REQUIRE(app.world_stack().size() == 1);
    CHECK(app.world_stack()[0].world == &app.main_world());
    World &a = app.create_world("a");
    const WorldEntry one[] = {{&a, true, true, true}};
    app.set_world_stack(one);
    h.ticks(1);
    app.set_world_stack({});
    h.ticks(1);
    CHECK(app.res<Trace>().joined() == "sim@a sim@main");
    CHECK(app.focused_world() == &app.main_world());
}

TEST_CASE("worlds: a stack set in FrameBegin Once < 0 applies in the same frame") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    World &a = app.create_world("a");
    app.insert_resource<NextStack>(NextStack{{{&a, true, true, true}}, false});
    app.add_system(desc(Stage::FrameBegin, install_stack, "t.install", -2000, SystemScope::Once));
    app.add_system(Stage::Simulate, tick_world, "t.sim");
    h.ticks(1);
    CHECK(app.res<Trace>().joined() == "sim@a");
}

TEST_CASE(
    "worlds: a stack set in TickBegin Once applies to that tick; in Update, to the next frame") {
    SUBCASE("TickBegin") {
        test::HeadlessApp h;
        App &app = h.app();
        app.insert_resource<Trace>();
        World &a = app.create_world("a");
        app.insert_resource<NextStack>(NextStack{{{&a, true, true, true}}, false});
        app.add_system(
            desc(Stage::TickBegin, install_stack, "t.install", -2000, SystemScope::Once));
        app.add_system(Stage::Simulate, tick_world, "t.sim");
        app.add_system(Stage::Update, update_world, "t.up");
        h.ticks(1);
        CHECK(app.res<Trace>().joined() == "sim@a up@a");
    }
    SUBCASE("Update") {
        test::HeadlessApp h;
        App &app = h.app();
        app.insert_resource<Trace>();
        World &a = app.create_world("a");
        app.insert_resource<NextStack>(NextStack{{{&a, true, true, true}}, false});
        app.add_system(desc(Stage::Update, install_stack, "t.install", 0, SystemScope::Once));
        app.add_system(Stage::Simulate, tick_world, "t.sim");
        app.add_system(Stage::Update, update_world, "t.up");
        h.ticks(2);
        CHECK(app.res<Trace>().joined() == "sim@main up@main sim@a up@a");
    }
}

TEST_CASE("worlds: create_world seeds, resources, prefabs and unique ids") {
    test::HeadlessApp h;
    App &app = h.app();
    const std::uint64_t seed = app.seed();
    CHECK(seed == 1);
    CHECK(app.main_world().seed() == derive_seed(seed, str_id("main").value ^ 0));
    World &a = app.create_world("level");
    World &b = app.create_world("level");
    CHECK(a.seed() == derive_seed(seed, str_id("level").value ^ 1));
    CHECK(b.seed() == derive_seed(seed, str_id("level").value ^ 2));
    CHECK(a.seed() != b.seed());
    CHECK(a.id() != b.id());
    CHECK(a.id() != app.main_world().id());
    CHECK(a.name() == str_id("level"));
    for (World *w : {&app.main_world(), &a, &b}) {
        CHECK(w->resources().has<DebugDraw>());
        CHECK(w->prefabs() == &app.res<PrefabLibrary>());
        CHECK(w->events().try_get<WorldRestored>() != nullptr);
    }
    // set_seed (before run) reseeds every world with the same derivation.
    app.set_seed(99);
    CHECK(app.seed() == 99);
    CHECK(app.main_world().seed() == derive_seed(99, str_id("main").value ^ 0));
    CHECK(b.seed() == derive_seed(99, str_id("level").value ^ 2));
}

TEST_CASE("worlds: --seed overrides AppConfig::seed") {
    const char *const argv[] = {"game", "--headless", "--seed", "1234"};
    AppConfig c;
    c.seed = 5;
    App app(c, 4, argv);
    CHECK(app.seed() == 1234);
    CHECK(app.main_world().seed() == derive_seed(1234, str_id("main").value));
}

TEST_CASE("worlds: destroy_world inside a pass is queued; the world leaves the stack") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    World &a = app.create_world("a");
    const std::uint32_t a_id = a.id();
    const WorldEntry stack[] = {{&app.main_world(), true, true, false}, {&a, true, true, true}};
    app.set_world_stack(stack);
    app.insert_resource<Doomed>(Doomed{&a});
    SystemDesc only = desc(Stage::Update, update_world, "t.only_a", 0, SystemScope::PerWorld);
    only.only_world = a_id;
    app.add_system(only);
    // Runs on main first (bottom of the stack); a must still be alive for its own Simulate pass.
    SystemDesc d = desc(Stage::Simulate, destroy_in_system, "t.destroy", 0, SystemScope::PerWorld);
    d.only_world = app.main_world().id();
    app.add_system(d);
    app.add_system(Stage::Simulate, tick_world, "t.sim", 10);
    h.ticks(1);
    // a still ticked and presented in the frame that queued its destruction.
    CHECK(app.res<Trace>().joined() == "destroy@main sim@main sim@a up@a");
    REQUIRE(app.world_stack().size() == 1);
    CHECK(app.world_stack()[0].world == &app.main_world());
    app.res<Trace>().lines.clear();
    h.ticks(1);
    CHECK(app.res<Trace>().joined() == "sim@main");
    CHECK(app.schedule_text().find("t.only_a") == std::string::npos);
}

TEST_CASE("worlds: destroy_world outside a frame is immediate") {
    test::HeadlessApp h;
    App &app = h.app();
    World &a = app.create_world("a");
    SystemDesc only = desc(Stage::Update, update_world, "t.only_a", 0, SystemScope::PerWorld);
    only.only_world = a.id();
    app.add_system(only);
    app.destroy_world(a);
    CHECK(app.schedule_text().find("t.only_a") == std::string::npos);
}

TEST_CASE("worlds: the main world cannot be destroyed and stacks are bounded") {
    // LAM_ASSERT: Release builds ignore the call instead.
    CHECK(apptest::dies([] {
              test::HeadlessApp h;
              h.app().destroy_world(h.app().main_world());
          }) == static_cast<bool>(LAM_ENABLE_ASSERTS));
    CHECK(apptest::dies([] {
        test::HeadlessApp h;
        std::vector<WorldEntry> entries;
        for (int i = 0; i <= kMaxWorlds; ++i)
            entries.push_back({&h.app().create_world("w"), true, true, true});
        h.app().set_world_stack(entries);
    }));
}

TEST_CASE("worlds: Ctx::time is per world") {
    test::HeadlessApp h;
    App &app = h.app();
    World &a = app.create_world("a");
    const WorldEntry stack[] = {{&app.main_world(), true, true, true}, {&a, false, true, false}};
    app.set_world_stack(stack);
    h.ticks(3);
    CHECK(app.time().tick == 3);
    CHECK(app.time().app_tick == 3);
    CHECK(app.time().frame == 2);
    CHECK(app.main_world().tick() == 3);
    CHECK(a.tick() == 0);
}
