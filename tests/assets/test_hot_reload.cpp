// Hot reload by mtime polling. Only in LAM_DEV builds and asynchronous mode.
#include <string>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/test/support.h>

#include "asset_test_util.h"

using namespace lam;
using namespace assets_test;
using Kind = AssetEvent<Node>::Kind;

namespace {

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

#if LAM_DEV

TEST_CASE("hot reload: a changed file reloads its asset and emits Modified") {
    HotFixture f;
    write(f.dir, "a.node", "value: one");
    const AssetId<Node> a = f.s().load<Node>("a.node");
    f.s().wait_all();
    f.s().update();
    CHECK(f.s().version(a) == 1);
    rewrite(f.dir, "a.node", "value: two");
    REQUIRE(pump_until(f.s(), [&] { return f.s().version(a) == 2; }));
    CHECK(f.s().get(a)->value == "two");
    CHECK(events<Node>(*f.app).back() == std::pair{Kind::Modified, a});
    CHECK(g_journal.with('U') == std::vector<std::string>{"a.node"});
    // Nothing changes: no further reloads.
    for (int i = 0; i < 20; ++i)
        f.s().update();
    CHECK(f.s().version(a) == 2);
}

TEST_CASE("hot reload: dependants re-finalize after their dependency reloads") {
    HotFixture f;
    write(f.dir, "p.node", "value: P\nnode: c.node");
    write(f.dir, "c.node", "value: C1");
    write(f.dir, "u.node", "value: U");
    const AssetId<Node> p = f.s().load<Node>("p.node");
    const AssetId<Node> u = f.s().load<Node>("u.node");
    f.s().wait_all();
    const auto c = AssetId<Node>::from_bits(f.s().get(p)->deps[0]);
    g_journal.clear();
    rewrite(f.dir, "c.node", "value: C2");
    REQUIRE(pump_until(f.s(), [&] { return f.s().version(p) == 2; }));
    CHECK(f.s().version(c) == 2);
    CHECK(f.s().version(u) == 1);
    CHECK(f.s().get(c)->value == "C2");
    CHECK(g_journal.with('F') == std::vector<std::string>{"c.node", "p.node"});
}

TEST_CASE("hot reload: files read through LoadContext::read are watched") {
    HotFixture f;
    write(f.dir, "a.node", "value: A\nread: data/extra.txt");
    write(f.dir, "data/extra.txt", "first");
    const AssetId<Node> a = f.s().load<Node>("a.node");
    f.s().wait_all();
    CHECK(f.s().get(a)->extra == "first");
    rewrite(f.dir, "data/extra.txt", "second");
    REQUIRE(pump_until(f.s(), [&] { return f.s().version(a) == 2; }));
    CHECK(f.s().get(a)->extra == "second");
}

TEST_CASE("hot reload: a failed reload keeps the old value; the next good save applies") {
    HotFixture f;
    test::CaptureLog log;
    write(f.dir, "a.node", "value: one");
    const AssetId<Node> a = f.s().load<Node>("a.node");
    f.s().wait_all();
    const std::size_t events_before = events<Node>(*f.app).size();
    rewrite(f.dir, "a.node", "fail: decode");
    REQUIRE(pump_until(f.s(), [&] { return g_journal.count('D') == 2 && f.s().pending() == 0; }));
    CHECK(f.s().state(a) == LoadState::Ready);
    CHECK(f.s().get(a)->value == "one");
    CHECK(f.s().version(a) == 1);
    CHECK(events<Node>(*f.app).size() == events_before);
    CHECK(log.contains("reload failed"));
    rewrite(f.dir, "a.node", "value: three", 20);
    REQUIRE(pump_until(f.s(), [&] { return f.s().version(a) == 2; }));
    CHECK(f.s().get(a)->value == "three");
}

TEST_CASE("hot reload: a missing asset loads when its file appears (Loaded)") {
    HotFixture f;
    test::CaptureLog log;
    const AssetId<Node> m = f.s().load<Node>("later.node");
    f.s().wait_all();
    CHECK(f.s().state(m) == LoadState::Failed);
    write(f.dir, "later.node", "value: here");
    REQUIRE(pump_until(f.s(), [&] { return f.s().state(m) == LoadState::Ready; }));
    CHECK(f.s().get(m)->value == "here");
    CHECK(events<Node>(*f.app).back() == std::pair{Kind::Loaded, m});
}

TEST_CASE("hot reload: changes during a reload coalesce into one more reload") {
    HotFixture f;
    write(f.dir, "a.node", "value: v0");
    const AssetId<Node> a = f.s().load<Node>("a.node");
    f.s().wait_all();
    rewrite(f.dir, "a.node", "value: v1\nsleep: 60", 10);
    f.s().update(); // reload 1 starts (slow decode)
    rewrite(f.dir, "a.node", "value: v2\nsleep: 5", 20);
    f.s().update();
    rewrite(f.dir, "a.node", "value: v3", 30);
    f.s().update();
    REQUIRE(pump_until(f.s(), [&] {
        return f.s().get(a) != nullptr && f.s().get(a)->value == "v3" && f.s().pending() == 0;
    }));
    CHECK(g_journal.count('D') <= 3); // initial + the slow reload + one coalesced reload
}

TEST_CASE("hot reload: unloaded assets are no longer watched") {
    HotFixture f;
    write(f.dir, "a.node", "value: one");
    const AssetScope scene = f.s().begin_scope();
    const AssetId<Node> a = f.s().load<Node>("a.node", scene);
    f.s().wait_all();
    f.s().end_scope(scene);
    f.s().update();
    CHECK(f.s().state(a) == LoadState::None);
    rewrite(f.dir, "a.node", "value: two");
    for (int i = 0; i < 20; ++i)
        f.s().update();
    CHECK(g_journal.count('D') == 1);
    CHECK(f.s().pending() == 0);
}

#endif // LAM_DEV

TEST_CASE("hot reload: off in synchronous (deterministic) mode") {
    test::TempDir dir;
    test::HeadlessApp h;
    AssetsConfig c;
    c.root = dir.path();
    c.hot_reload = true;
    c.watch_interval_seconds = 0.0f;
    h.add<AssetsPlugin>(c);
    AssetServer &s = h.app().res<AssetServer>();
    add_doc_loaders(s);
    write(dir, "a.node", "value: one");
    const AssetId<Node> a = s.load<Node>("a.node");
    rewrite(dir, "a.node", "value: two");
    h.frames(10);
    CHECK(s.version(a) == 1);
    CHECK(s.get(a)->value == "one");
}

TEST_CASE("hot reload: compiled out of Release builds (LAM_DEV == 0)") {
    HotFixture f;
    write(f.dir, "a.node", "value: one");
    const AssetId<Node> a = f.s().load<Node>("a.node");
    f.s().wait_all();
    rewrite(f.dir, "a.node", "value: two");
    for (int i = 0; i < 10; ++i)
        f.s().update();
    if constexpr (LAM_DEV == 0) {
        CHECK(f.s().version(a) == 1);
    } else {
        // Debug builds reload; give the worker time.
        CHECK(pump_until(f.s(), [&] { return f.s().version(a) == 2; }));
    }
}
