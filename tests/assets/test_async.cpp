// The asset server in asynchronous mode (not --headless), with one decode worker.
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/test/support.h>

#include "asset_test_util.h"

using namespace lam;
using namespace assets_test;
using Kind = AssetEvent<Node>::Kind;

TEST_CASE("async: load returns at once; the worker decodes; update() finalizes") {
    AsyncFixture f(async_config());
    REQUIRE(!f.s().synchronous());
    write(f.dir, "a.node", "value: A\nsleep: 20");
    const int off_main = g_off_main_decodes.load();
    const AssetId<Node> a = f.s().load<Node>("a.node");
    REQUIRE(a);
    CHECK(a.index == 1);
    CHECK(f.s().state(a) != LoadState::Ready);
    CHECK(f.s().get(a) == nullptr);
    CHECK(f.s().pending() == 1);
    CHECK(!f.s().all_ready());
    CHECK(f.s().load<Node>("a.node") == a); // in flight: same id
    REQUIRE(pump_until(f.s(), [&] { return f.s().state(a) == LoadState::Ready; }));
    CHECK(f.s().get(a)->value == "A");
    CHECK(f.s().pending() == 0);
    CHECK(g_off_main_decodes.load() == off_main + 1);
    CHECK(events<Node>(*f.app) == std::vector{std::pair{Kind::Loaded, a}});
}

TEST_CASE("async: finalize follows load order even when a later asset decodes first") {
    AsyncFixture f(async_config());
    GateGuard gate;
    write(f.dir, "a.node", "value: A\nnode: slow.node");
    write(f.dir, "slow.node", "value: S\ngate");
    write(f.dir, "b.node", "value: B");
    const AssetId<Node> a = f.s().load<Node>("a.node");
    const AssetId<Node> b = f.s().load<Node>("b.node");
    // No update() yet, so a's dependency is not queued: the worker decodes a, then b.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_journal.count('D') < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(g_journal.count('D') == 2);
    // update() queues slow.node, which waits at the gate; b is decoded but must wait for a.
    f.s().update();
    REQUIRE(g_gate.wait_waiting(1));
    for (int i = 0; i < 5; ++i)
        f.s().update();
    CHECK(f.s().state(b) != LoadState::Ready);
    CHECK(f.s().state(a) != LoadState::Ready);
    CHECK(f.s().pending() == 3);
    g_gate.open();
    f.s().wait_all();
    CHECK(f.s().pending() == 0);
    CHECK(g_journal.with('F') == std::vector<std::string>{"slow.node", "a.node", "b.node"});
    const auto ev = events<Node>(*f.app);
    REQUIRE(ev.size() == 3);
    CHECK(ev[1].second == a);
    CHECK(ev[2].second == b);
}

TEST_CASE("async: the finalize budget spreads finalizes over frames (at least one per update)") {
    AsyncFixture f(async_config(0.0f));
    for (int i = 0; i < 5; ++i)
        write(f.dir, "n" + std::to_string(i) + ".node", "value: x");
    std::vector<AssetId<Node>> ids;
    for (int i = 0; i < 5; ++i)
        ids.push_back(f.s().load<Node>("n" + std::to_string(i) + ".node"));
    // Wait until the worker decoded everything (without finalizing anything).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_journal.count('D') < 5 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(g_journal.count('D') == 5);
    for (int frame = 1; frame <= 5; ++frame) {
        f.s().update();
        const auto ready = std::count_if(ids.begin(), ids.end(), [&](AssetId<Node> id) {
            return f.s().state(id) == LoadState::Ready;
        });
        CHECK(ready == frame);
    }
    CHECK(f.s().pending() == 0);
}

TEST_CASE("async: a generous budget finalizes everything decoded in one update") {
    AsyncFixture f(async_config(1000.0f));
    for (int i = 0; i < 5; ++i)
        write(f.dir, "n" + std::to_string(i) + ".node", "value: x");
    for (int i = 0; i < 5; ++i)
        f.s().load<Node>("n" + std::to_string(i) + ".node");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_journal.count('D') < 5 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    f.s().update();
    CHECK(f.s().pending() == 0);
}

TEST_CASE("async: load_now blocks until the asset and its dependencies are Ready") {
    AsyncFixture f(async_config());
    write(f.dir, "first.node", "value: F\nsleep: 30");
    write(f.dir, "p.node", "value: P\nnode: c.node");
    write(f.dir, "c.node", "value: C\nnode: g.node");
    write(f.dir, "g.node", "value: G");
    const AssetId<Node> first = f.s().load<Node>("first.node");
    Result<AssetId<Node>> p = f.s().load_now<Node>("p.node");
    REQUIRE(p);
    CHECK(f.s().state(*p) == LoadState::Ready);
    const auto c = AssetId<Node>::from_bits(f.s().get(*p)->deps[0]);
    CHECK(f.s().state(c) == LoadState::Ready);
    f.s().wait_all();
    CHECK(f.s().state(first) == LoadState::Ready);
    test::CaptureLog log;
    write(f.dir, "bad.node", "value: B\nnode: missing.node");
    Result<AssetId<Node>> bad = f.s().load_now<Node>("bad.node");
    REQUIRE(!bad);
    CHECK(bad.error().message.find("missing.node") != std::string::npos);
    CHECK(f.s().pending() == 0);
}

TEST_CASE("async: reload blocks, swaps the value and reloads dependants") {
    AsyncFixture f(async_config());
    write(f.dir, "p.node", "value: P\nnode: c.node");
    write(f.dir, "c.node", "value: C1");
    const AssetId<Node> p = f.s().load<Node>("p.node");
    f.s().wait_all();
    const auto c = AssetId<Node>::from_bits(f.s().get(p)->deps[0]);
    write(f.dir, "c.node", "value: C2");
    REQUIRE(f.s().reload(c).ok());
    CHECK(f.s().get(c)->value == "C2");
    CHECK(f.s().version(c) == 2);
    f.s().wait_all(); // the dependant's reload is asynchronous
    CHECK(f.s().version(p) == 2);
    CHECK(f.s().get(p)->deps[0] == c.bits());
    test::CaptureLog log;
    write(f.dir, "c.node", "fail: decode");
    CHECK(!f.s().reload(c));
    CHECK(f.s().get(c)->value == "C2");
    write(f.dir, "fresh.node", "value: X\nsleep: 20");
    const AssetId<Node> fresh = f.s().load<Node>("fresh.node");
    Status busy = f.s().reload(fresh);
    CHECK(!busy);
    CHECK(busy.error().code == ErrorCode::Busy);
    f.s().wait_all();
}

TEST_CASE("async: failures and cycles leave nothing pending") {
    AsyncFixture f(async_config());
    test::CaptureLog log;
    write(f.dir, "a.node", "value: A\nnode: b.node");
    write(f.dir, "b.node", "value: B\nnode: a.node");
    write(f.dir, "p.node", "value: P\nnode: missing.node\nnode: ok.node");
    write(f.dir, "ok.node", "value: OK\nsleep: 10");
    write(f.dir, "fin.node", "fail: finalize");
    const AssetId<Node> a = f.s().load<Node>("a.node");
    const AssetId<Node> p = f.s().load<Node>("p.node");
    const AssetId<Node> fin = f.s().load<Node>("fin.node");
    f.s().wait_all();
    CHECK(f.s().pending() == 0);
    CHECK(f.s().state(a) == LoadState::Failed);
    CHECK(f.s().state(p) == LoadState::Failed);
    CHECK(f.s().error(p)->message.find("dependency 'missing.node' failed") != std::string::npos);
    CHECK(f.s().state(fin) == LoadState::Failed);
    f.s().end_scope(kRootScope);
    f.s().collect_garbage();
    CHECK(f.s().state(a) == LoadState::None);
    CHECK(f.s().state(p) == LoadState::None);
    CHECK(f.s().state(fin) == LoadState::None);
}

TEST_CASE("async: a cached dependency pinned by a running decode survives GC") {
    AsyncFixture f(async_config());
    GateGuard gate;
    write(f.dir, "x.node", "value: X");
    write(f.dir, "parent.node", "value: P\nnode: x.node\ngate");
    const AssetScope scene = f.s().begin_scope();
    const AssetId<Node> x = f.s().load<Node>("x.node", scene);
    f.s().wait_all();
    const AssetId<Node> parent = f.s().load<Node>("parent.node");
    REQUIRE(g_gate.wait_waiting(1)); // depend(x) ran: x is pinned
    f.s().end_scope(scene);
    f.s().collect_garbage();
    CHECK(f.s().state(x) == LoadState::Ready); // pinned, not unloaded
    g_gate.open();
    f.s().wait_all();
    CHECK(f.s().state(parent) == LoadState::Ready);
    CHECK(f.s().get(parent)->deps[0] == x.bits());
    f.s().collect_garbage();
    CHECK(f.s().state(x) == LoadState::Ready); // now retained by the parent
}

TEST_CASE("async: many parents sharing dependencies, with main-thread loads racing the worker") {
    AsyncFixture f(async_config());
    for (int i = 0; i < 10; ++i)
        write(f.dir, "s" + std::to_string(i) + ".node", "value: S" + std::to_string(i));
    for (int i = 0; i < 60; ++i) {
        std::string body = "value: P" + std::to_string(i) + "\n";
        for (int k = 0; k < 4; ++k)
            body += "node: s" + std::to_string((i + k * 3) % 10) + ".node\n";
        write(f.dir, "p" + std::to_string(i) + ".node", body);
    }
    const int live = g_live.load();
    const AssetScope scene = f.s().begin_scope();
    std::vector<AssetId<Node>> parents;
    for (int i = 0; i < 60; ++i) {
        parents.push_back(f.s().load<Node>("p" + std::to_string(i) + ".node", scene));
        if (i % 6 == 0) f.s().load<Node>("s" + std::to_string(i % 10) + ".node", scene);
        f.s().update();
    }
    f.s().wait_all();
    CHECK(f.s().pending() == 0);
    for (int i = 0; i < 60; ++i) {
        const Node *n = f.s().get(parents[i]);
        REQUIRE(n != nullptr);
        CHECK(n->deps.size() == 4);
        for (std::uint64_t d : n->deps)
            CHECK(f.s().state(AssetId<Node>::from_bits(d)) == LoadState::Ready);
    }
    f.s().end_scope(scene);
    f.s().collect_garbage();
    for (const AssetId<Node> p : parents)
        CHECK(f.s().state(p) == LoadState::None);
    CHECK(g_live.load() == live);
}

TEST_CASE("async: destroying the server with loads pending cancels them without leaks") {
    const int live = g_live.load();
    {
        AsyncFixture f(async_config());
        g_gate.close();
        write(f.dir, "g1.node", "value: G\ngate");
        write(f.dir, "n1.node", "value: N\nnode: n2.node");
        write(f.dir, "n2.node", "value: N2");
        f.s().load<Node>("g1.node");
        f.s().load<Node>("n1.node");
        REQUIRE(g_gate.wait_waiting(1));
        CHECK(f.s().pending() >= 2);
        // The worker is inside a decode: let it finish while the App shuts the server down.
        std::thread opener([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            g_gate.open();
        });
        f.app.reset();
        opener.join();
    }
    CHECK(g_live.load() == live);
}

TEST_CASE("async: ending the root scope stops the worker; later loads run inline") {
    AsyncFixture f(async_config());
    GateGuard gate;
    write(f.dir, "g.node", "value: G\ngate");
    write(f.dir, "q.node", "value: Q");
    const AssetId<Node> g = f.s().load<Node>("g.node");
    const AssetId<Node> q = f.s().load<Node>("q.node");
    REQUIRE(g_gate.wait_waiting(1));
    std::thread opener([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        g_gate.open();
    });
    f.s().end_scope(kRootScope);
    opener.join();
    CHECK(f.s().pending() == 0);
    CHECK(f.s().state(q) == LoadState::Failed);
    CHECK(f.s().error(q)->code == ErrorCode::Busy);
    CHECK(f.s().synchronous());
    f.s().collect_garbage();
    CHECK(f.s().state(g) == LoadState::None);
    write(f.dir, "late.node", "value: L");
    const AssetId<Node> late = f.s().load<Node>("late.node");
    CHECK(f.s().state(late) == LoadState::Ready);
}

namespace {

struct StartupIds {
    AssetId<Node> id;
};

void startup_load(Ctx &ctx) {
    ctx.res<StartupIds>().id = ctx.res<AssetServer>().load<Node>("slow.node");
}

} // namespace

TEST_CASE("async: wait_for_startup_assets holds ticks until startup loads finalize") {
    AsyncFixture f(async_config());
    GateGuard gate;
    write(f.dir, "slow.node", "value: S\ngate");
    f.app->insert_resource<StartupIds>();
    f.app->add_startup(startup_load, "test.startup_load");
    f.app->run_frames(5);
    CHECK(f.app->tick() == 0);
    CHECK(f.s().pending() == 1);
    g_gate.open();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.app->tick() == 0 && std::chrono::steady_clock::now() < deadline) {
        f.app->run_frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(f.app->tick() > 0);
    CHECK(f.s().state(f.app->res<StartupIds>().id) == LoadState::Ready);
}

TEST_CASE("async: without wait_for_startup_assets ticks run while loading") {
    test::TempDir dir;
    AppConfig cfg;
    cfg.settings_file = "";
    cfg.wait_for_startup_assets = false;
    App app(std::move(cfg));
    AssetsConfig ac = async_config();
    ac.root = dir.path();
    app.add_plugin<AssetsPlugin>(ac);
    add_doc_loaders(app.res<AssetServer>());
    GateGuard gate;
    write(dir, "slow.node", "value: S\ngate");
    app.insert_resource<StartupIds>();
    app.add_startup(startup_load, "test.startup_load");
    app.run_frames(3);
    CHECK(app.tick() > 0);
    CHECK(app.res<AssetServer>().pending() == 1);
}

TEST_CASE("sync: startup loads are Ready before the first tick") {
    test::TempDir dir;
    test::HeadlessApp h;
    h.add<AssetsPlugin>(config_for(dir));
    add_doc_loaders(h.app().res<AssetServer>());
    write(dir, "slow.node", "value: S");
    h.app().insert_resource<StartupIds>();
    h.app().add_startup(startup_load, "test.startup_load");
    h.frames(1);
    CHECK(h.app().tick() == 1);
    CHECK(h.app().res<AssetServer>().state(h.app().res<StartupIds>().id) == LoadState::Ready);
}

namespace {

struct SeenEvents {
    int loaded = 0;
    int modified = 0;
};

void count_events(Ctx &ctx) {
    for (const AssetEvent<Node> &e : ctx.read<AssetEvent<Node>>()) {
        if (e.kind == AssetEvent<Node>::Kind::Loaded) ++ctx.res<SeenEvents>().loaded;
        if (e.kind == AssetEvent<Node>::Kind::Modified) ++ctx.res<SeenEvents>().modified;
    }
}

} // namespace

TEST_CASE("async: AssetEvents from asset_update reach frame systems exactly once") {
    AsyncFixture f(async_config());
    f.app->insert_resource<SeenEvents>();
    SystemDesc d;
    d.stage = Stage::Update;
    d.fn = &count_events;
    d.name = "test.count_events";
    d.scope = SystemScope::Once;
    f.app->add_system(d);
    for (int i = 0; i < 4; ++i)
        write(f.dir, "e" + std::to_string(i) + ".node", "value: x");
    for (int i = 0; i < 4; ++i)
        f.s().load<Node>("e" + std::to_string(i) + ".node");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.s().pending() > 0 && std::chrono::steady_clock::now() < deadline) {
        f.app->run_frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    f.app->run_frames(3);
    CHECK(f.app->res<SeenEvents>().loaded == 4);
}

TEST_CASE("steady state: asset_update allocates nothing (sync and async with hot reload)") {
    {
        test::TempDir dir;
        test::HeadlessApp h;
        h.add<AssetsPlugin>(config_for(dir));
        add_doc_loaders(h.app().res<AssetServer>());
        write(dir, "a.node", "value: A");
        h.app().res<AssetServer>().load<Node>("a.node");
        test::run_ticks(h.app(), 120);
        CHECK_NO_ALLOC(test::run_ticks(h.app(), 600));
    }
    {
        AssetsConfig c = async_config();
        c.hot_reload = true;
        c.watch_interval_seconds = 0.0f; // stat every watched file every frame
        AsyncFixture f(c);
        for (int i = 0; i < 8; ++i)
            write(f.dir, "w" + std::to_string(i) + ".node", "value: x\nread: shared.txt");
        write(f.dir, "shared.txt", "s");
        for (int i = 0; i < 8; ++i)
            f.s().load<Node>("w" + std::to_string(i) + ".node");
        f.s().wait_all();
        test::run_ticks(*f.app, 120);
        CHECK_NO_ALLOC(test::run_ticks(*f.app, 600));
        AssetServer &s = f.s();
        CHECK_NO_ALLOC(for (int i = 0; i < 100; ++i) s.update());
    }
}

TEST_CASE("async: pending(scope) counts the scope's loads and their dependencies once") {
    AsyncFixture f(async_config());
    GateGuard gate;
    write(f.dir, "a.node", "value: A\nnode: slow.node");
    write(f.dir, "b.node", "value: B\nnode: slow.node");
    write(f.dir, "slow.node", "value: S\ngate");
    write(f.dir, "other.node", "value: O\ngate");
    const AssetScope scene = f.s().begin_scope();
    const AssetScope idle = f.s().begin_scope();
    CHECK(f.s().pending(scene) == 0);
    const AssetId<Node> a = f.s().load<Node>("a.node", scene);
    const AssetId<Node> b = f.s().load<Node>("b.node", scene);
    const AssetId<Node> other = f.s().load<Node>("other.node", kRootScope); // unrelated load
    // The single worker decodes a and b, then waits at the gate inside other.node.
    REQUIRE(g_gate.wait_waiting(1));
    // update() sees a and b decoded and queues their shared dependency slow.node.
    f.s().update();
    CHECK(f.s().pending(scene) == 3); // a, b and slow.node (counted once)
    CHECK(f.s().pending(idle) == 0);  // nothing of its own
    // other.node only: the root scope is a scope like the others.
    CHECK(f.s().pending(kRootScope) == 1);
    CHECK(f.s().pending() == 4); // the global count
    g_gate.open();
    f.s().wait_all();
    CHECK(f.s().state(a) == LoadState::Ready);
    CHECK(f.s().state(b) == LoadState::Ready);
    CHECK(f.s().state(other) == LoadState::Ready);
    CHECK(f.s().pending(scene) == 0);
    CHECK(f.s().pending(kRootScope) == 0);
    f.s().end_scope(idle);
    f.s().end_scope(scene);
    CHECK(f.s().pending(scene) == 0); // an ended scope has nothing pending
}
