// lam_assets regressions. Each one failed before its fix.
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <lam/assets/asset_server.h>
#include <lam/core/platform.h>
#include <lam/test/support.h>

#include "asset_test_util.h"
#include "detail/asset_internal.h"

using namespace lam;
using namespace assets_test;

namespace {

// ---- A platform whose clock moves only when the test says so -----------------------------------

class StepPlatform final : public Platform {
public:
    double t = 0;
    bool should_close() override { return false; }
    double now_seconds() override { return t; }
    void begin_frame() override {}
    void end_frame() override {}
    WindowInfo window() const override {
        return WindowInfo{{640, 360}, {640, 360}, 1, true, false, {0, 0, 640, 360}};
    }
    void set_fullscreen(bool) override {}
    bool has_gpu() const override { return false; }
};

// Tick 3 ends the only scope holding a.node; tick 4 loads b.node, then a.node again.
struct GcPlan {
    AssetScope scope = kRootScope;
    AssetId<Node> a, b;
};

void gc_plan_system(Ctx &ctx) {
    GcPlan &p = ctx.res<GcPlan>();
    AssetServer &s = ctx.res<AssetServer>();
    if (ctx.app().tick() == 3) s.end_scope(p.scope);
    if (ctx.app().tick() == 4) {
        p.b = s.load<Node>("b.node", kRootScope);
        p.a = s.load<Node>("a.node", kRootScope);
    }
}

// A windowed (not --headless) App with synchronous assets whose frames run the given numbers of
// ticks (like --record-input, which may run several ticks per frame). Returns (a, b) bits.
std::pair<std::uint64_t, std::uint64_t> run_gc_plan(std::initializer_list<int> ticks_per_frame) {
    test::TempDir dir;
    write(dir, "a.node", "value: A");
    write(dir, "b.node", "value: B");
    AppConfig c;
    c.tick_rate = 64;
    c.settings_file = "";
    c.max_ticks_per_frame = 4;
    App app(std::move(c));
    auto platform = std::make_unique<StepPlatform>();
    StepPlatform *clock = platform.get();
    app.set_platform(std::move(platform));
    AssetsConfig ac = config_for(dir);
    ac.synchronous = true;
    app.add_plugin<AssetsPlugin>(ac);
    AssetServer &s = app.res<AssetServer>();
    add_doc_loaders(s);
    GcPlan &plan = app.insert_resource<GcPlan>();
    plan.scope = s.begin_scope();
    REQUIRE(s.load<Node>("a.node", plan.scope));
    app.add_system(SystemDesc{.stage = Stage::TickBegin, .fn = &gc_plan_system, .name = "test.gc_plan",
                              .order = 0, .scope = SystemScope::Once});
    app.run_frames(1); // clock origin: no tick
    for (int n : ticks_per_frame) {
        clock->t += static_cast<double>(n) / 64.0;
        app.run_frames(1);
    }
    CHECK(app.tick() == 5);
    CHECK(s.state(plan.a) == LoadState::Ready);
    CHECK(s.state(plan.b) == LoadState::Ready);
    return {plan.a.bits(), plan.b.bits()};
}

// ---- A loader that makes a runtime asset in finalize --------------------------------------------

struct Maker {
    std::string value;
    AssetId<LeafDoc> made;                                       // runtime asset made by finalize
};
inline constexpr std::string_view kMakerExt[] = {".maker"};

class MakerLoader final : public AssetLoader<Maker> {
public:
    std::span<const std::string_view> extensions() const override { return kMakerExt; }
    Result<Maker> decode(std::span<const std::byte> bytes, LoadContext &) override {
        Maker m;
        m.value.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return m;
    }
    Status finalize(Maker &m, FinalizeContext &ctx) override {
        LeafDoc made;
        made.value = "made from " + m.value;
        m.made = ctx.server.add<LeafDoc>(std::move(made)); // into the current scope
        return {};
    }
};

// Visits a "scene" three times: begin a scope, load m.maker into it while it is current (as
// SceneStack does during enter()), restore the root, finish loading, end the scope, collect.
// Returns how many of the runtime LeafDocs made by finalize are still alive.
int leaked_after_visits(AssetServer &s, bool async) {
    int alive = 0;
    AssetId<LeafDoc> made[3];
    for (int i = 0; i < 3; ++i) {
        const AssetScope scene = s.begin_scope();
        s.set_current_scope(scene);
        const AssetId<Maker> m = s.load<Maker>("m.maker");
        s.set_current_scope(kRootScope);
        if (async) s.wait_all();
        REQUIRE(s.get(m) != nullptr);
        made[i] = s.get(m)->made;
        CHECK(s.get(made[i]) != nullptr);
        s.end_scope(scene);
        s.collect_garbage();
        if (async) s.update();
        CHECK(s.state(m) == LoadState::None);
    }
    for (const AssetId<LeafDoc> id : made)
        alive += s.get(id) != nullptr ? 1 : 0;
    return alive;
}

// ---- Hot reload ---------------------------------------------------------------------------------

struct HotFixture {
    test::TempDir dir;
    std::unique_ptr<App> app = make_async_app();
    AssetServer *server = nullptr;
    HotFixture() {
        g_journal.clear();
        AssetsConfig c;
        c.root = dir.path();
        c.hot_reload = true;
        c.watch_interval_seconds = 0.0f; // every watched file, every update
        app->add_plugin<AssetsPlugin>(c);
        server = &app->res<AssetServer>();
        add_doc_loaders(*server);
    }
    AssetServer &s() { return *server; }
};

} // namespace

TEST_CASE("synchronous GC runs at every tick boundary, so ids do not depend on frame pacing") {
    // One tick per frame (a --headless replay) and two ticks in one frame (a windowed
    // --record-input run) must hand out the same ids: a.node, released at tick 3, is unloaded
    // before tick 4 in both, so tick 4's loads see the same free list.
    const auto one = run_gc_plan({1, 1, 1, 1, 1});
    const auto grouped = run_gc_plan({1, 1, 2, 1});
    CHECK(one == grouped);
    CHECK(AssetId<Node>::from_bits(one.first).generation == 1); // a.node was reloaded fresh
}

TEST_CASE("runtime assets made in finalize live and die with the asset, in both modes") {
    SUBCASE("synchronous") {
        SyncFixture f;
        f.s().register_loader<Maker>(std::make_unique<MakerLoader>());
        write(f.dir, "m.maker", "M");
        CHECK(leaked_after_visits(f.s(), false) == 0);
    }
    SUBCASE("asynchronous") {
        AsyncFixture f(async_config());
        f.s().register_loader<Maker>(std::make_unique<MakerLoader>());
        write(f.dir, "m.maker", "M");
        CHECK(leaked_after_visits(f.s(), true) == 0); // was 3: retained by the root scope forever
    }
    SUBCASE("synchronous: a cache hit in a second scope keeps the runtime asset alive") {
        // Scene A loads the asset, scene B hits the cache, then A ends. The runtime asset made by
        // finalize must survive too; it used to be held by A's scope only.
        SyncFixture f;
        f.s().register_loader<Maker>(std::make_unique<MakerLoader>());
        write(f.dir, "m.maker", "M");
        const AssetScope a = f.s().begin_scope();
        f.s().set_current_scope(a);
        const AssetId<Maker> m = f.s().load<Maker>("m.maker");
        const AssetScope b = f.s().begin_scope();
        f.s().set_current_scope(b);
        CHECK(f.s().load<Maker>("m.maker") == m);
        f.s().end_scope(a);
        f.s().collect_garbage();
        REQUIRE(f.s().get(m) != nullptr);
        CHECK(f.s().get(f.s().get(m)->made) != nullptr);
        CHECK(f.s().current_scope() == b); // finalize restored the scope it found
        // A reload replaces the runtime asset: the old one goes with the old value.
        const AssetId<LeafDoc> old = f.s().get(m)->made;
        REQUIRE(f.s().reload(m).ok());
        f.s().collect_garbage();
        CHECK(f.s().get(old) == nullptr);
        CHECK(f.s().get(f.s().get(m)->made) != nullptr);
        f.s().end_scope(b);
        f.s().collect_garbage();
        CHECK(f.s().state(m) == LoadState::None);
    }
}

TEST_CASE("named runtime assets report their name, unnamed ones \"\"") {
    SyncFixture f;
    Node n;
    n.value = "page";
    const AssetId<Node> page = f.s().add<Node>(n, "@atlas/default/0");
    const AssetId<Node> unnamed = f.s().add<Node>(n);
    CHECK(f.s().path_of(page) == "@atlas/default/0");
    AssetResolver &resolver = f.s();
    CHECK(resolver.path_of(type_id<Node>(), page.bits()) == "@atlas/default/0"); // saves write the name
    CHECK(resolver.resolve(type_id<Node>(), "@atlas/default/0") == page.bits()); // and load back to it
    CHECK(resolver.path_of(type_id<Node>(), unnamed.bits()) == "");                // saved as null
    CHECK(f.s().load<Node>("@atlas/default/0") == page);
    write(f.dir, "a.node", "value: A");
    CHECK(f.s().path_of(f.s().load<Node>("a.node")) == "a.node"); // loaded assets keep their path
}

#if LAM_DEV

TEST_CASE("a parent failed by a missing dependency loads once the dependency appears") {
    test::CaptureLog log;
    SUBCASE("the dependency is only the parent's") {
        HotFixture f;
        write(f.dir, "level.node", "value: L\nleaf: tiles.leaf");
        const AssetId<Node> level = f.s().load<Node>("level.node");
        f.s().wait_all();
        for (int i = 0; i < 5; ++i)
            f.s().update(); // the failed dependency is collected (and unwatched)
        REQUIRE(f.s().state(level) == LoadState::Failed);
        write(f.dir, "tiles.leaf", "value: T");
        REQUIRE(pump_until(f.s(), [&] { return f.s().state(level) == LoadState::Ready; }, 3000));
        const Node *l = f.s().get(level);
        REQUIRE(l->deps.size() == 1);
        const LeafDoc *t = f.s().get(AssetId<LeafDoc>::from_bits(l->deps[0]));
        REQUIRE(t != nullptr);
        CHECK(t->value == "T");
    }
    SUBCASE("the dependency is also loaded on its own (and reloads first)") {
        HotFixture f;
        const AssetId<LeafDoc> tiles = f.s().load<LeafDoc>("tiles.leaf");
        write(f.dir, "level.node", "value: L\nleaf: tiles.leaf");
        const AssetId<Node> level = f.s().load<Node>("level.node");
        f.s().wait_all();
        f.s().update();
        REQUIRE(f.s().state(level) == LoadState::Failed);
        REQUIRE(f.s().state(tiles) == LoadState::Failed);
        write(f.dir, "tiles.leaf", "value: T");
        REQUIRE(pump_until(f.s(), [&] {
            return f.s().state(tiles) == LoadState::Ready && f.s().state(level) == LoadState::Ready;
        }, 3000));
    }
}

#endif // LAM_DEV

TEST_CASE("a freed slot whose generation reaches UINT32_MAX is retired at once") {
    std::uint32_t g = 1;
    CHECK(detail::advance_generation(g));
    CHECK(g == 2);
    g = 0xFFFFFFFDu;
    CHECK(detail::advance_generation(g)); // 0xFFFFFFFE may still be handed out
    CHECK(g == 0xFFFFFFFEu);
    CHECK_FALSE(detail::advance_generation(g)); // 0xFFFFFFFF never is: the index is not reused
    CHECK(g == detail::kRetiredGeneration);
    CHECK_FALSE(detail::advance_generation(g)); // retired stays retired
    CHECK(g == detail::kRetiredGeneration);
}

TEST_CASE("a load error names the file once, also when the parser names it") {
    test::CaptureLog log;
    HotFixture f;
    write(f.dir, "named.node", "fail: named");
    write(f.dir, "plain.node", "fail: decode");
    Result<AssetId<Node>> named = f.s().load_now<Node>("named.node");
    REQUIRE(!named);
    CHECK(named.error().message == "named.node: bad content");
    Result<AssetId<Node>> plain = f.s().load_now<Node>("plain.node");
    REQUIRE(!plain);
    CHECK(plain.error().message == "plain.node: bad content");
    CHECK(detail::name_file("a.json", "a.json:3: x") == "a.json:3: x");
    CHECK(detail::name_file("a.json", "a.jsonx: y") == "a.json: a.jsonx: y");
}
