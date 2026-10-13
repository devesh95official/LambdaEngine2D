// lam_scene regressions. Each one failed before its fix.
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/data.h>
#include <lam/core/fs.h>
#include <lam/core/platform.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/world.h>
#include <lam/scene/save.h>
#include <lam/scene/scene.h>
#include <lam/test/support.h>

namespace srf {
struct Spin {
    float v = 0;
};
struct Coins {
    float total = 0;
};
} // namespace srf
LAM_REFLECT(srf::Spin, v)
LAM_REFLECT(srf::Coins, total)
namespace srf_other {
struct Spin {                                                   // same short name as srf::Spin
    int turns = 0;
};
} // namespace srf_other
LAM_REFLECT(srf_other::Spin, turns)

using namespace lam;
using namespace lam::literals;

namespace {

std::vector<std::string> g_events;

// A scene whose enter() spawns through the world's command buffer and whose first TickBegin
// PerWorld pass records how many entities it sees.
struct SpawnSeen {
    int first = -1;
};
void record_spawned(Ctx &ctx) {
    SpawnSeen &s = ctx.local<SpawnSeen>();
    if (s.first < 0) s.first = static_cast<int>(ctx.world().entity_count());
}
int g_seen_at_first_tick = -2;

class Level final : public Scene {
public:
    std::string_view name() const override { return "level"; }
    void enter(App &app, World &w) override {
        w.resources().insert<SpawnSeen>();
        w.commands().spawn();
        w.commands().spawn();
        add_system(app, Stage::TickBegin, record_spawned, "test.record_spawned", -1900);
    }
    void exit(App &, World &w) override { g_seen_at_first_tick = w.resources().get<SpawnSeen>().first; }
};

struct Count {
    int n = 0;
};
// At "title"'s 4th tick: app tick 4, the first tick of a 3-tick frame when paced at 3 (the first
// frame runs none), so the replace is applied at TickBegin of tick 5 there and at FrameBegin when
// paced at 1.
void replace_at_fourth(Ctx &ctx) {
    if (++ctx.local<Count>().n != 4) return;
    ctx.res<SceneStack>().replace(std::make_unique<Level>()); // no fade
}

class Title final : public Scene {
public:
    std::string_view name() const override { return "title"; }
    void enter(App &app, World &w) override {
        w.resources().insert<Count>();
        add_system(app, Stage::Simulate, replace_at_fourth, "test.replace");
    }
};

// Runs ~40 ticks at `ticks_per_frame` (NullPlatform clock, not headless); returns what the Level
// world's first TickBegin pass saw.
int paced_spawn_run(int ticks_per_frame) {
    g_seen_at_first_tick = -2;
    {
        AppConfig cfg;
        cfg.settings_file = "";
        cfg.max_ticks_per_frame = 4;
        App app(cfg);
        app.set_platform(std::make_unique<NullPlatform>(60 / ticks_per_frame));
        app.add_plugin<ScenePlugin>();
        app.res<SceneStack>().push(std::make_unique<Title>());
        int frames = 0;
        while (app.tick() < 40 && frames++ < 1000)
            app.run_frames(1);
    }
    return g_seen_at_first_tick;
}

class Named final : public Scene {
public:
    explicit Named(std::string n) : name_(std::move(n)) {}
    std::string_view name() const override { return name_; }
    void enter(App &, World &) override { g_events.push_back("enter:" + name_); }
    void exit(App &, World &) override { g_events.push_back("exit:" + name_); }
    void resumed(App &, const SceneMessage *) override { g_events.push_back("resumed:" + name_); }

private:
    std::string name_;
};

std::string events_text() {
    std::string s;
    for (const std::string &e : g_events)
        s += e + " ";
    return s;
}

bool any_input(const App &app) {
    for (const WorldEntry &e : app.world_stack())
        if (e.input) return true;
    return false;
}

} // namespace

TEST_CASE("enter-time commands land before the new world's first stage, at any pacing") {
    const int one = paced_spawn_run(1);
    const int three = paced_spawn_run(3);
    CHECK(one == 2);
    CHECK(three == 2);                                          // was 0: the replace ran at TickBegin
}

TEST_CASE("commands queued in resumed() are applied by the pop") {
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    SceneStack &stack = h.app().res<SceneStack>();
    struct Base final : Scene {
        std::string_view name() const override { return "base"; }
        void enter(App &, World &) override {}
        void resumed(App &, const SceneMessage *) override { world().commands().spawn(); }
    };
    stack.push(std::make_unique<Base>());
    stack.push(std::make_unique<Named>("menu"));
    h.frames(1);
    stack.pop();
    stack.apply(h.app());                                       // outside any stage
    REQUIRE(stack.size() == 1);
    CHECK(stack.top()->world().commands().pending() == 0);
    CHECK(stack.top()->world().entity_count() == 1);
}

TEST_CASE("a save nested deeper than the limit is a Parse error, not a crash") {
    std::string deep = R"({"format":"lam-save","game":"hop","version":1,"summary":{"x":)";
    const int levels = 100000;
    deep.append(levels, '[');
    deep.append(levels, ']');
    deep += R"(},"data":{}})";
    Result<DataDocument> doc = DataDocument::parse(deep, "slot1.json");
    REQUIRE_FALSE(doc);
    CHECK(doc.error().code == ErrorCode::Parse);
    CHECK(doc.error().message.find("slot1.json") != std::string::npos);
    Result<SaveEnvelope> env = read_save(deep, "hop", 1, {});
    CHECK_FALSE(env);
    // Brackets inside strings do not count; the limit itself parses.
    std::string ok = "{\"s\":\"" + std::string(1000, '[') + "\",\"a\":";
    ok.append(DataDocument::kMaxDataDepth - 1, '[');
    ok.append(DataDocument::kMaxDataDepth - 1, ']');
    ok += "}";
    CHECK(DataDocument::parse(ok, "ok.json"));
    test::TempDir dir;
    SaveSlots slots(dir.path(), "hop", 1, 2);
    REQUIRE(write_file_atomic(slots.slot_path(1), deep));
    CHECK(slots.summaries()[0].empty());                        // skipped, no crash
    CHECK_FALSE(slots.load(1));
}

TEST_CASE("negative zero survives a world save and load") {
    World w;
    const Entity e = w.create();
    w.add<srf::Spin>(e).v = -0.0f;
    Result<std::string> text = serialize_world_text(w, {});
    REQUIRE(text);
    CHECK(text->find("-0.0") != std::string::npos);
    World back;
    REQUIRE(deserialize_world_text(back, *text, {}, "save.json"));
    const float v = back.get<srf::Spin>(e).v;
    CHECK(v == 0.0f);
    CHECK(std::signbit(v));                                     // was +0: "-0" read as an integer
    Result<std::string> again = serialize_world_text(back, {});
    REQUIRE(again);
    CHECK(*again == *text);
    DataWriter dw;
    dw.begin_object().key("z").value(-0.0).end_object();
    Result<std::string> z = dw.finish();
    REQUIRE(z);
    CHECK(*z == R"({"z":-0.0})");
}

TEST_CASE("destroying a scene's world removes the scene from the stack") {
    g_events.clear();
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    App &app = h.app();
    SceneStack &stack = app.res<SceneStack>();
    stack.push(std::make_unique<Named>("a"));
    stack.push(std::make_unique<Named>("b"));
    h.frames(1);
    REQUIRE(stack.size() == 2);
    World &wb = stack.top()->world();
    app.destroy_world(wb);
    h.frames(1);
    CHECK(events_text() == "enter:a enter:b exit:b ");
    REQUIRE(stack.size() == 1);                                 // was 2: the dead entry stayed
    CHECK(stack.top()->name() == "a");
    for (const WorldEntry &e : stack.world_entries())
        CHECK(e.world != &wb);
    stack.pop();                                                // pops "a", not the dead entry
    h.frames(1);
    CHECK(events_text() == "enter:a enter:b exit:b exit:a ");
    CHECK(stack.empty());
}

TEST_CASE("ScenePlugin added before AssetsPlugin shuts down cleanly") {
    test::TempDir dir;
    REQUIRE(write_file_atomic(dir.file("level.txt"), "L"));
    struct Loader final : Scene {
        std::string_view name() const override { return "loader"; }
        void enter(App &app, World &) override { (void)app.res<AssetServer>().load<TextAsset>("level.txt"); }
    };
    {
        test::HeadlessApp h;
        h.add<ScenePlugin>();                                   // before AssetsPlugin
        h.add<AssetsPlugin>(AssetsConfig{.root = dir.path()});
        h.app().res<SceneStack>().push(std::make_unique<Loader>());
        h.frames(2);
        REQUIRE(h.app().res<SceneStack>().top()->asset_scope() != kRootScope);
    } // was: LAM_ASSERT "scope was never begun or already ended" in SceneStack::shutdown
    CHECK(true);
}

TEST_CASE("a save whose component name is shared by another reflected type is refused") {
    World w;
    const Entity e = w.create();
    w.add<srf_other::Spin>(e).turns = 3;                        // registered after srf::Spin
    Result<std::string> text = serialize_world_text(w, {});
    REQUIRE_FALSE(text);
    CHECK(text.error().message.find("shares its name") != std::string::npos);
    World ok;
    ok.add<srf::Coins>(ok.create()).total = 2;
    CHECK(serialize_world_text(ok, {}));
}

TEST_CASE("no world has input while a transition runs on an empty stack") {
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    App &app = h.app();
    SceneStack &stack = app.res<SceneStack>();
    const Transition fade{.kind = TransitionKind::Fade, .ticks = 3};
    // The first push with a fade: the fade-out runs before any scene exists.
    stack.push(std::make_unique<Named>("game"), fade);
    h.frames(1);
    CHECK(stack.transitioning());
    CHECK_FALSE(any_input(app));                                // was: the main world had input
    h.frames(6);
    CHECK_FALSE(stack.transitioning());
    CHECK(any_input(app));
    // Pop to empty with a fade: the fade-in runs on the main world, deaf.
    stack.pop(std::nullopt, fade);
    for (int f = 0; f < 5; ++f) {
        h.frames(1);
        if (stack.transitioning()) CHECK_FALSE(any_input(app));
    }
    h.frames(3);
    CHECK_FALSE(stack.transitioning());
    CHECK(stack.empty());
    REQUIRE(app.world_stack().size() == 1);
    CHECK(app.world_stack()[0].world == &app.main_world());
    CHECK(app.world_stack()[0].input);                          // input back once the fade ended
}
