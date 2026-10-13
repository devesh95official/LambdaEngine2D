// Stage order, scopes, system-list changes during a pass and the schedule printout.
#include <lam/test/support.h>

#include "app_test_util.h"

using namespace lam;
using apptest::Trace;

namespace {

#define LAM_TRACE_SYSTEM(fn, label)                                                                \
    void fn(Ctx &ctx) {                                                                            \
        apptest::trace(ctx, label);                                                                \
    }

LAM_TRACE_SYSTEM(fb_once_neg, "fb.once-")
LAM_TRACE_SYSTEM(fb_world, "fb.world")
LAM_TRACE_SYSTEM(fb_once_pos, "fb.once+")
LAM_TRACE_SYSTEM(tb_once_pos, "tb.once+")
LAM_TRACE_SYSTEM(tb_world, "tb.world")
LAM_TRACE_SYSTEM(input_world, "input")
LAM_TRACE_SYSTEM(sim_world, "sim")
LAM_TRACE_SYSTEM(phys_world, "phys")
LAM_TRACE_SYSTEM(post_world, "post")
LAM_TRACE_SYSTEM(anim_world, "anim")
LAM_TRACE_SYSTEM(te_world, "te.world")
LAM_TRACE_SYSTEM(te_once_neg, "te.once-")
LAM_TRACE_SYSTEM(up_once_neg, "up.once-")
LAM_TRACE_SYSTEM(up_world, "up.world")
LAM_TRACE_SYSTEM(up_once_pos, "up.once+")
LAM_TRACE_SYSTEM(r_once_neg, "r.once-")
LAM_TRACE_SYSTEM(r_world, "r.world")
LAM_TRACE_SYSTEM(r_once_pos, "r.once+")
LAM_TRACE_SYSTEM(ui_once_neg, "ui.once-")
LAM_TRACE_SYSTEM(ui_world, "ui.world")
LAM_TRACE_SYSTEM(ui_once_pos, "ui.once+")
LAM_TRACE_SYSTEM(fe_once, "fe.once")
LAM_TRACE_SYSTEM(sys_a, "a")
LAM_TRACE_SYSTEM(sys_b, "b")
LAM_TRACE_SYSTEM(sys_c, "c")
LAM_TRACE_SYSTEM(sys_d, "d")
LAM_TRACE_SYSTEM(sys_late, "late")
LAM_TRACE_SYSTEM(sys_only, "only")

SystemDesc desc(Stage s, SystemFn fn, std::string_view name, int order, SystemScope scope) {
    SystemDesc d;
    d.stage = s;
    d.fn = fn;
    d.name = name;
    d.order = order;
    d.scope = scope;
    return d;
}

void record_stage(Ctx &ctx) {
    ctx.res<Trace>().add(std::string(stage_name(ctx.stage())) +
                         (ctx.scope() == SystemScope::Once ? ":once" : ":world"));
}

// Adds sys_late (Simulate, order -100) the first time it runs.
void adder(Ctx &ctx) {
    apptest::trace(ctx, "adder");
    struct Done {};
    if (ctx.try_res<Done>()) return;
    ctx.app().insert_resource<Done>();
    ctx.app().add_system(Stage::Simulate, sys_late, "t.late", -100);
}

// FrameBegin Once -2000 (like scene apply): adds a PerWorld FrameBegin system.
void scene_apply_like(Ctx &ctx) {
    struct Done {};
    if (ctx.try_res<Done>()) return;
    ctx.app().insert_resource<Done>();
    ctx.app().add_system(
        desc(Stage::FrameBegin, fb_world, "t.fb_world_added", -1000, SystemScope::PerWorld));
}

struct RemoveTarget {
    std::uint32_t world = 0;
};
void remover(Ctx &ctx) {
    apptest::trace(ctx, "remover");
    ctx.app().remove_systems_of(ctx.res<RemoveTarget>().world);
}

bool never(const Ctx &) {
    return false;
}

} // namespace

TEST_CASE("app: one frame runs every stage in order with the right scopes") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    using S = Stage;
    constexpr auto Once = SystemScope::Once;
    constexpr auto World = SystemScope::PerWorld;
    // Registered out of order on purpose.
    app.add_system(desc(S::FrameEnd, fe_once, "t.fe", 0, Once));
    app.add_system(desc(S::RenderUI, ui_once_pos, "t.ui+", 5, Once));
    app.add_system(desc(S::RenderUI, ui_world, "t.uiw", 0, World));
    app.add_system(desc(S::RenderUI, ui_once_neg, "t.ui-", -5, Once));
    app.add_system(desc(S::Render, r_once_pos, "t.r+", 0, Once));
    app.add_system(desc(S::Render, r_world, "t.rw", 0, World));
    app.add_system(desc(S::Render, r_once_neg, "t.r-", -1, Once));
    app.add_system(desc(S::Update, up_once_pos, "t.up+", 3, Once));
    app.add_system(desc(S::Update, up_world, "t.upw", 0, World));
    app.add_system(desc(S::Update, up_once_neg, "t.up-", -3, Once));
    app.add_system(desc(S::TickEnd, te_once_neg, "t.te-", -500, Once)); // still after the loop
    app.add_system(desc(S::TickEnd, te_world, "t.tew", 0, World));
    app.add_system(desc(S::Animate, anim_world, "t.anim", 0, World));
    app.add_system(desc(S::PostPhysics, post_world, "t.post", 0, World));
    app.add_system(desc(S::Physics, phys_world, "t.phys", 0, World));
    app.add_system(desc(S::Simulate, sim_world, "t.sim", 0, World));
    app.add_system(desc(S::Input, input_world, "t.input", 0, World));
    app.add_system(desc(S::TickBegin, tb_world, "t.tbw", -3000, World));
    app.add_system(desc(S::TickBegin, tb_once_pos, "t.tb+", 500, Once)); // still before the loop
    app.add_system(desc(S::FrameBegin, fb_once_pos, "t.fb+", 0, Once));
    app.add_system(desc(S::FrameBegin, fb_world, "t.fbw", 0, World));
    app.add_system(desc(S::FrameBegin, fb_once_neg, "t.fb-", -1, Once));
    h.ticks(1);
    CHECK(
        app.res<Trace>().joined() ==
        "fb.once-@main fb.world@main fb.once+@main tb.once+@main tb.world@main input@main sim@main "
        "phys@main post@main anim@main te.world@main te.once-@main up.once-@main up.world@main "
        "up.once+@main r.once-@main r.world@main r.once+@main ui.once-@main ui.world@main "
        "ui.once+@main fe.once@main");
}

TEST_CASE("app: Ctx reports the stage and scope it runs in") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    app.add_system(desc(Stage::TickBegin, record_stage, "t.tb_once", 0, SystemScope::Once));
    app.add_system(desc(Stage::Simulate, record_stage, "t.sim", 0, SystemScope::PerWorld));
    app.add_system(desc(Stage::FrameEnd, record_stage, "t.fe", 0, SystemScope::Once));
    h.ticks(1);
    CHECK(app.res<Trace>().joined() == "TickBegin:once Simulate:world FrameEnd:once");
}

TEST_CASE("app: the short add_system overload picks Once for FrameEnd and PerWorld elsewhere") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    app.add_system(Stage::FrameEnd, record_stage, "t.fe");
    app.add_system(Stage::Update, record_stage, "t.up");
    h.frames(1);
    CHECK(app.res<Trace>().joined() == "Update:world FrameEnd:once");
}

TEST_CASE("app: order ties are broken by registration index") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    app.add_system(Stage::Simulate, sys_a, "t.a", 0);
    app.add_system(Stage::Simulate, sys_b, "t.b", -5);
    app.add_system(Stage::Simulate, sys_c, "t.c", 0);
    app.add_system(Stage::Simulate, sys_d, "t.d", -5);
    h.ticks(2);
    CHECK(app.res<Trace>().joined() == "b@main d@main a@main c@main b@main d@main a@main c@main");
}

TEST_CASE("app: systems added during a pass run from the next pass on") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    app.add_system(Stage::Simulate, adder, "t.adder", 0);
    h.ticks(2);
    // Tick 1: only the adder (the new system is queued); tick 2: the new one first (order -100).
    CHECK(app.res<Trace>().joined() == "adder@main late@main adder@main");
}

TEST_CASE("app: a system added at FrameBegin Once -2000 runs in the same frame's PerWorld pass") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    app.add_system(
        desc(Stage::FrameBegin, scene_apply_like, "t.scene_apply", -2000, SystemScope::Once));
    h.frames(1);
    CHECK(app.res<Trace>().joined() == "fb.world@main");
}

TEST_CASE("app: remove_systems_of during a pass is applied when the pass ends") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    World &w = app.create_world("other");
    app.insert_resource<RemoveTarget>(RemoveTarget{w.id()});
    const WorldEntry stack[] = {{&w, true, true, true}};
    app.set_world_stack(stack);
    SystemDesc only = desc(Stage::Simulate, sys_only, "t.only", 10, SystemScope::PerWorld);
    only.only_world = w.id();
    app.add_system(only);
    app.add_system(Stage::Simulate, remover, "t.remover", 0);
    h.ticks(2);
    CHECK(app.res<Trace>().joined() == "remover@other only@other remover@other");
}

TEST_CASE("app: only_world and run_if filter calls") {
    test::HeadlessApp h;
    App &app = h.app();
    app.insert_resource<Trace>();
    World &w = app.create_world("second");
    const WorldEntry stack[] = {{&app.main_world(), true, true, false}, {&w, true, true, true}};
    app.set_world_stack(stack);
    SystemDesc only = desc(Stage::Simulate, sys_only, "t.only", 0, SystemScope::PerWorld);
    only.only_world = w.id();
    app.add_system(only);
    SystemDesc skipped = desc(Stage::Simulate, sys_a, "t.never", 0, SystemScope::PerWorld);
    skipped.run_if = never;
    app.add_system(skipped);
    h.ticks(1);
    CHECK(app.res<Trace>().joined() == "only@second");
}

TEST_CASE("app: schedule_text lists the engine systems in run order") {
    test::HeadlessApp h;
    App &app = h.app();
    app.add_system(Stage::Simulate, sys_a, "game.move", 7);
    const std::string s = app.schedule_text();
    const auto at = [&](std::string_view needle) { return s.find(needle); };
    REQUIRE(at("platform_begin_frame") != std::string::npos);
    CHECK(at("platform_begin_frame") < at("settings_autosave"));
    CHECK(at("settings_autosave") < at("copy_prev_transforms"));
    CHECK(at("copy_prev_transforms") < at("game.move"));
    CHECK(at("game.move") < at("propagate_parents"));
    CHECK(at("lifetimes") < at("settle_spawned"));
    CHECK(at("settle_spawned") < at("align_pools<Transform,PrevTransform>"));
    CHECK(at("align_pools<Transform,PrevTransform>") < at("hash_log"));
    CHECK(at("hash_log") < at("platform_end_frame"));
    CHECK(s.find("[lam.app]") != std::string::npos);
    CHECK(s.find("[game]") != std::string::npos);
    CHECK(s.find("game.move  [game]") != std::string::npos);
}

TEST_CASE("app: --print-schedule logs the schedule and run() exits without frames") {
    test::CaptureLog log;
    const char *const argv[] = {"game", "--headless", "--print-schedule"};
    App app({}, 3, argv);
    app.insert_resource<Trace>();
    app.add_system(Stage::Simulate, sys_a, "t.a", 0);
    CHECK(app.run() == 0);
    CHECK(app.quitting());
    CHECK(app.tick() == 0);
    CHECK(app.res<Trace>().lines.empty());
    CHECK(log.contains("platform_end_frame"));
    CHECK(log.contains("t.a"));
}

TEST_CASE("app: disallowed scopes fail LAM_CHECK") {
    std::string out;
    CHECK(apptest::dies(
        [] {
            test::HeadlessApp h;
            h.app().add_system(desc(Stage::Simulate, sys_a, "t.bad", 0, SystemScope::Once));
        },
        &out));
    CHECK(out.find("not allowed") != std::string::npos);
    CHECK(apptest::dies([] {
        test::HeadlessApp h;
        h.app().add_system(desc(Stage::FrameEnd, sys_a, "t.bad", 0, SystemScope::PerWorld));
    }));
    CHECK(apptest::dies([] {
        test::HeadlessApp h;
        h.app().add_system(desc(Stage::Physics, sys_a, "t.bad", 0, SystemScope::Once));
    }));
    CHECK_FALSE(apptest::dies([] {
        test::HeadlessApp h;
        h.app().add_system(desc(Stage::Update, sys_a, "t.ok1", 0, SystemScope::Once));
        h.app().add_system(desc(Stage::TickEnd, sys_a, "t.ok2", 0, SystemScope::Once));
        h.app().add_system(desc(Stage::FrameBegin, sys_a, "t.ok3", 0, SystemScope::PerWorld));
    }));
}
