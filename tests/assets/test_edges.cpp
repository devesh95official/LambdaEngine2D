// Asset server edge cases, in both modes.
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

namespace {

std::size_t decodes_of(std::string_view path) {
    const std::vector<std::string> d = g_journal.with('D');
    return static_cast<std::size_t>(std::count(d.begin(), d.end(), std::string(path)));
}

} // namespace

TEST_CASE("edges: a main-thread load of a path a running decode just depended on shares the id") {
    const int live = g_live.load();
    {
        AsyncFixture f(async_config());
        GateGuard gate;
        write(f.dir, "parent.node", "value: P\nnode: y.node\ngate");
        write(f.dir, "y.node", "value: Y");
        const AssetId<Node> parent = f.s().load<Node>("parent.node");
        // depend(y) ran on the worker: y has a pinned registry entry but no slot yet.
        REQUIRE(g_gate.wait_waiting(1));
        const AssetId<Node> y = f.s().load<Node>("y.node");
        CHECK(y.index == 2);
        CHECK(f.s().state(y) == LoadState::Queued);
        g_gate.open();
        f.s().wait_all();
        REQUIRE(f.s().state(parent) == LoadState::Ready);
        CHECK(f.s().get(parent)->deps == std::vector<std::uint64_t>{y.bits()});
        CHECK(f.s().state(y) == LoadState::Ready);
        CHECK(decodes_of("y.node") == 1);
        CHECK(g_journal.with('F') == std::vector<std::string>{"y.node", "parent.node"});
        // Two references (the root scope and the parent), released in either order.
        f.s().end_scope(kRootScope);
        f.s().collect_garbage();
        CHECK(f.s().state(parent) == LoadState::None);
        CHECK(f.s().state(y) == LoadState::None);
    }
    CHECK(g_live.load() == live);
}

TEST_CASE("edges: a scope that ends while its load is in flight is collected after it lands") {
    AsyncFixture f(async_config());
    GateGuard gate;
    write(f.dir, "slow.node", "value: S\ngate");
    const AssetScope scene = f.s().begin_scope();
    const AssetId<Node> id = f.s().load<Node>("slow.node", scene);
    REQUIRE(g_gate.wait_waiting(1));
    f.s().end_scope(scene);
    f.s().update(); // GC skips a load in flight
    CHECK(f.s().state(id) != LoadState::None);
    CHECK(f.s().pending() == 1);
    g_gate.open();
    REQUIRE(pump_until(f.s(), [&] { return f.s().state(id) == LoadState::None; }));
    CHECK(f.s().pending() == 0);
    CHECK(events<Node>(*f.app) ==
          std::vector{std::pair{Kind::Loaded, id}, std::pair{Kind::Removed, id}});
    CHECK(g_journal.with('U') == std::vector<std::string>{"slow.node"});
}

TEST_CASE("edges: a failed load whose scope ended mid-load is collected too (sync and async)") {
    test::CaptureLog log;
    {
        AsyncFixture f(async_config());
        GateGuard gate;
        write(f.dir, "bad.node", "gate\nfail: decode");
        const AssetScope scene = f.s().begin_scope();
        const AssetId<Node> id = f.s().load<Node>("bad.node", scene);
        REQUIRE(g_gate.wait_waiting(1));
        f.s().end_scope(scene);
        f.s().update();
        g_gate.open();
        REQUIRE(pump_until(f.s(), [&] { return f.s().state(id) == LoadState::None; }));
        CHECK(events<Node>(*f.app) ==
              std::vector{std::pair{Kind::Failed, id}, std::pair{Kind::Removed, id}});
    }
}

TEST_CASE("edges: a reload that would close a dependency cycle fails and keeps the old value") {
    test::CaptureLog log;
    const int live = g_live.load();
    for (const bool async : {false, true}) {
        CAPTURE(async);
        std::unique_ptr<SyncFixture> sf;
        std::unique_ptr<AsyncFixture> af;
        AssetServer *s = nullptr;
        const test::TempDir *dir = nullptr;
        App *app = nullptr;
        if (async) {
            af = std::make_unique<AsyncFixture>(async_config());
            s = &af->s();
            dir = &af->dir;
            app = af->app.get();
        } else {
            sf = std::make_unique<SyncFixture>();
            s = &sf->s();
            dir = &sf->dir;
            app = &sf->h.app();
        }
        write(*dir, "r.node", "value: R1");
        write(*dir, "b.node", "value: B\nnode: r.node");
        const AssetId<Node> r = s->load<Node>("r.node");
        s->wait_all();
        REQUIRE(s->state(r) == LoadState::Ready);
        write(*dir, "r.node", "value: R2\nnode: b.node"); // r -> b -> r
        const Status st = s->reload(r);
        REQUIRE(!st);
        CHECK(st.error().message.find("cycle") != std::string::npos);
        s->wait_all();
        CHECK(s->state(r) == LoadState::Ready);
        CHECK(s->get(r)->value == "R1");
        CHECK(s->version(r) == 1);
        CHECK(s->pending() == 0);
        // b failed and nothing retains it: the next GC removes it; r is unaffected.
        s->collect_garbage();
        const auto removed = events<Node>(*app);
        CHECK(std::count_if(removed.begin(), removed.end(),
                            [](const auto &e) { return e.first == Kind::Removed; }) == 1);
        // c already depends on q: reloading q to depend on c would close a cycle of Ready
        // assets (and reload_dependants would ping-pong between them forever).
        write(*dir, "q.node", "value: Q1");
        write(*dir, "c.node", "value: C\nnode: q.node");
        const AssetId<Node> q = s->load<Node>("q.node");
        const AssetId<Node> c = s->load<Node>("c.node");
        s->wait_all();
        REQUIRE(s->state(c) == LoadState::Ready);
        write(*dir, "q.node", "value: Q2\nnode: c.node");
        const Status st2 = s->reload(q);
        REQUIRE(!st2);
        CHECK(st2.error().message.find("cycle") != std::string::npos);
        s->wait_all();
        CHECK(s->get(q)->value == "Q1");
        CHECK(s->version(q) == 1);
        CHECK(s->version(c) == 1);
        CHECK(s->pending() == 0);
        s->end_scope(kRootScope);
        s->collect_garbage();
        CHECK(s->state(r) == LoadState::None);
    }
    CHECK(g_live.load() == live);
}

TEST_CASE("edges: shutdown with a decoded parent, a decode in progress and queued dependencies") {
    const int live = g_live.load();
    {
        AsyncFixture f(async_config());
        GateGuard gate; // opens before the fixture is destroyed, also when a REQUIRE fails
        write(f.dir, "p.node", "value: P\nnode: g.node\nnode: x.node\nnode: y.node");
        write(f.dir, "g.node", "value: G\ngate");
        write(f.dir, "x.node", "value: X\nnode: z.node");
        write(f.dir, "y.node", "value: Y");
        write(f.dir, "z.node", "value: Z");
        const AssetId<Node> p = f.s().load<Node>("p.node");
        REQUIRE(wait_decodes(1));
        f.s().update(); // p decoded: g, x and y are queued; the worker enters g and waits
        REQUIRE(g_gate.wait_waiting(1));
        CHECK(f.s().pending() == 4);
        CHECK(f.s().state(p) == LoadState::Loading);
        std::thread opener([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            g_gate.open();
        });
        f.app.reset(); // AssetsPlugin::shutdown joins the worker after g, cancels the rest
        opener.join();
        CHECK(g_journal.count('F') == 0);
    }
    CHECK(g_live.load() == live);
}

#if LAM_DEV
TEST_CASE("edges: shutdown with a hot reload in flight") {
    const int live = g_live.load();
    {
        AssetsConfig c = async_config();
        c.hot_reload = true;
        c.watch_interval_seconds = 0.0f;
        AsyncFixture f(c);
        write(f.dir, "a.node", "value: A1\nnode: d.node");
        write(f.dir, "d.node", "value: D");
        const AssetId<Node> a = f.s().load<Node>("a.node");
        f.s().wait_all();
        REQUIRE(f.s().state(a) == LoadState::Ready);
        GateGuard gate;
        rewrite(f.dir, "a.node", "value: A2\ngate\nnode: d.node\nnode: e.node");
        write(f.dir, "e.node", "value: E");
        f.s().update(); // the poll queues the reload; the worker waits inside its decode
        REQUIRE(g_gate.wait_waiting(1));
        CHECK(f.s().pending() == 1);
        CHECK(f.s().get(a)->value == "A1"); // the old value serves meanwhile
        std::thread opener([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            g_gate.open();
        });
        f.app.reset();
        opener.join();
    }
    CHECK(g_live.load() == live);
}
#endif

namespace {

struct FirstTick {
    AssetId<Node> id;
    int ticks = 0;
    bool ready_at_first_tick = false;
};

void startup_request(Ctx &ctx) {
    ctx.res<FirstTick>().id = ctx.res<AssetServer>().load<Node>("level.node");
}

void tick_probe(Ctx &ctx) {
    FirstTick &ft = ctx.app().res<FirstTick>();
    if (ft.ticks++ == 0)
        ft.ready_at_first_tick = ctx.app().res<AssetServer>().state(ft.id) == LoadState::Ready;
}

void add_probe(App &app) {
    app.insert_resource<FirstTick>();
    app.add_startup(startup_request, "test.startup_request");
    app.add_system(Stage::Simulate, tick_probe, "test.tick_probe");
}

} // namespace

TEST_CASE("edges: the first tick sees the startup assets Ready (HeadlessApp and asynchronous)") {
    {
        test::TempDir dir;
        test::HeadlessApp h;
        h.add<AssetsPlugin>(config_for(dir));
        add_doc_loaders(h.app().res<AssetServer>());
        write(dir, "level.node", "value: L\nnode: tiles.node");
        write(dir, "tiles.node", "value: T");
        add_probe(h.app());
        h.ticks(3);
        CHECK(h.app().res<FirstTick>().ticks == 3);
        CHECK(h.app().res<FirstTick>().ready_at_first_tick);
    }
    {
        AsyncFixture f(async_config());
        GateGuard gate;
        write(f.dir, "level.node", "value: L\nnode: tiles.node");
        write(f.dir, "tiles.node", "value: T\ngate");
        add_probe(*f.app);
        f.app->run_frames(4);
        CHECK(f.app->res<FirstTick>().ticks == 0);
        CHECK(f.app->tick() == 0);
        g_gate.open();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (f.app->res<FirstTick>().ticks == 0 && std::chrono::steady_clock::now() < deadline) {
            f.app->run_frames(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(f.app->res<FirstTick>().ticks > 0);
        CHECK(f.app->res<FirstTick>().ready_at_first_tick);
    }
}

TEST_CASE("edges: LoadContext outside a server decode loads plainly and reads next to its path") {
    SyncFixture f;
    write(f.dir, "c.node", "value: C");
    write(f.dir, "dir/extra.txt", "EXTRA");
    LoadContext ctx(f.s(), "dir/p.node", 0, 0);
    CHECK(ctx.path() == "dir/p.node");
    const AssetId<Node> c = ctx.depend<Node>("../c.node");
    REQUIRE(c);
    CHECK(f.s().state(c) == LoadState::Ready);
    CHECK(f.s().path_of(c) == "c.node");
    Result<std::vector<std::byte>> bytes = ctx.read("extra.txt");
    REQUIRE(bytes);
    CHECK(std::string(reinterpret_cast<const char *>(bytes->data()), bytes->size()) == "EXTRA");
    CHECK(!ctx.read("missing.txt"));
}

TEST_CASE("edges: asset_update runs at FrameBegin, Once, order -3000") {
    test::HeadlessApp h;
    h.add<AssetsPlugin>();
    const std::string schedule = h.app().schedule_text();
    const std::size_t at = schedule.find("asset_update");
    REQUIRE(at != std::string::npos);
    const std::size_t begin = schedule.rfind('\n', at) == std::string::npos ? 0 : schedule.rfind('\n', at) + 1;
    const std::string line = schedule.substr(begin, schedule.find('\n', at) - begin);
    CHECK(line.find("FrameBegin") != std::string::npos);
    CHECK(line.find("Once") != std::string::npos);
    CHECK(line.find("-3000") != std::string::npos);
    CHECK(line.find("[lam.assets]") != std::string::npos);
}

TEST_CASE("edges: deterministic mode never uses the worker (loads, dependencies, reloads)") {
    SyncFixture f;
    const int off_main = g_off_main_decodes.load();
    write(f.dir, "p.node", "value: P\nnode: c.node\nread: extra.txt");
    write(f.dir, "c.node", "value: C");
    write(f.dir, "extra.txt", "x");
    const AssetId<Node> p = f.s().load<Node>("p.node");
    REQUIRE(f.s().state(p) == LoadState::Ready);
    REQUIRE(f.s().load_now<Node>("c.node"));
    write(f.dir, "c.node", "value: C2");
    REQUIRE(f.s().reload(AssetId<Node>::from_bits(f.s().get(p)->deps[0])).ok());
    f.h.frames(5);
    f.s().wait_all();
    CHECK(g_journal.count('D') == 4); // p, c, c again, p again (dependant)
    CHECK(g_off_main_decodes.load() == off_main);
}
