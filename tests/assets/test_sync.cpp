// The asset server in synchronous mode (--headless), plus sync/async equivalence.
#include <algorithm>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/test/support.h>

#include "asset_test_util.h"

using namespace lam;
using namespace assets_test;
using Kind = AssetEvent<Node>::Kind;

namespace {

struct Fixture {
    test::TempDir dir;
    test::HeadlessApp h;
    AssetServer *server = nullptr;
    Fixture() {
        g_journal.clear();
        h.add<AssetsPlugin>(config_for(dir));
        server = &h.app().res<AssetServer>();
        add_doc_loaders(*server);
    }
    AssetServer &s() { return *server; }
};

} // namespace

TEST_CASE("sync: the server is synchronous under --headless, hot reload off, root from config") {
    Fixture f;
    CHECK(f.s().synchronous());
    CHECK(f.s().root() == f.dir.path());
    CHECK(f.h.app().asset_resolver() == &f.s());
    CHECK(f.s().current_scope() == kRootScope);
    AssetsConfig forced;
    forced.synchronous = false;
    forced.hot_reload = true;
    test::HeadlessApp h2;
    h2.add<AssetsPlugin>(forced);
    CHECK(h2.app().res<AssetServer>().synchronous()); // deterministic() forces it
}

TEST_CASE("sync: ids follow load() call order and assets are Ready at once") {
    Fixture f;
    write(f.dir, "a.node", "value: A");
    write(f.dir, "b.node", "value: B");
    write(f.dir, "c.node", "value: C");
    const AssetId<Node> a = f.s().load<Node>("a.node");
    const AssetId<Node> b = f.s().load<Node>("b.node");
    const AssetId<Node> c = f.s().load<Node>("c.node");
    CHECK(a.index == 1);
    CHECK(b.index == 2);
    CHECK(c.index == 3);
    CHECK(a.generation == 1);
    CHECK(f.s().state(a) == LoadState::Ready);
    REQUIRE(f.s().get(b) != nullptr);
    CHECK(f.s().get(b)->value == "B");
    CHECK(f.s().get(b)->finalized == 1);
    CHECK(f.s().version(c) == 1);
    CHECK(f.s().pending() == 0);
    CHECK(f.s().all_ready());
    const auto ev = events<Node>(f.h.app());
    REQUIRE(ev.size() == 3);
    CHECK(ev[0] == std::pair{Kind::Loaded, a});
    CHECK(ev[1] == std::pair{Kind::Loaded, b});
    CHECK(ev[2] == std::pair{Kind::Loaded, c});
    CHECK(f.s().path_of(b) == "b.node");
}

TEST_CASE("sync: the cache key is the normalised path; types and fragments are separate") {
    Fixture f;
    write(f.dir, "dir/a.node", "value: A");
    const AssetId<Node> a = f.s().load<Node>("dir/a.node");
    CHECK(f.s().load<Node>("dir/./a.node") == a);
    CHECK(f.s().load<Node>("other/../dir/a.node") == a);
    CHECK(f.s().load<Node>("dir\\a.node") == a);
    CHECK(f.s().load<Node>("dir//a.node") == a);
    CHECK(g_journal.count('D') == 1);
    const AssetId<Node> frag = f.s().load<Node>("dir/a.node#2");
    CHECK(frag != a);
    CHECK(f.s().path_of(frag) == "dir/a.node#2");
    CHECK(f.s().state(frag) == LoadState::Ready); // the fragment is not part of the file name
    CHECK(f.s().resolve_path("dir/a.node#2") == f.dir.path() + "/dir/a.node");
    CHECK(f.s().resolve_path("/abs/x.png") == "/abs/x.png");
    CHECK(f.s().path_of(AssetId<Node>{}) == "");
}

TEST_CASE("sync: a path without a loader gives id 0; load_now reports Unsupported") {
    Fixture f;
    write(f.dir, "a.unknown", "x");
    test::CaptureLog log;
    CHECK(!f.s().load<Node>("a.unknown"));
    CHECK(log.contains("no loader"));
    Result<AssetId<Node>> r = f.s().load_now<Node>("a.unknown");
    REQUIRE(!r);
    CHECK(r.error().code == ErrorCode::Unsupported);
    CHECK(!f.s().load<Node>(""));
    CHECK(f.s().pending() == 0);
}

TEST_CASE("sync: a missing file fails with NotFound and a Failed event; the id stays cached") {
    Fixture f;
    test::CaptureLog log;
    const AssetId<Node> m = f.s().load<Node>("missing.node");
    REQUIRE(m);
    CHECK(f.s().state(m) == LoadState::Failed);
    REQUIRE(f.s().error(m) != nullptr);
    CHECK(f.s().error(m)->code == ErrorCode::NotFound);
    CHECK(f.s().error(m)->message.find("missing.node") != std::string::npos);
    CHECK(f.s().get(m) == nullptr);
    CHECK(f.s().load<Node>("missing.node") == m);
    CHECK(events<Node>(f.h.app()).back() == std::pair{Kind::Failed, m});
    CHECK(log.count(LogLevel::Error) >= 1);
    Result<AssetId<Node>> r = f.s().load_now<Node>("missing.node");
    CHECK(!r);
}

TEST_CASE("sync: loader registration: case-insensitive, longest extension first, last wins") {
    Fixture f;
    static constexpr std::string_view kJson[] = {".json"};
    static constexpr std::string_view kSounds[] = {".sounds.json"};
    f.s().register_loader<Node>(std::make_unique<DocLoader<0>>(kJson, "json:"));
    f.s().register_loader<Node>(std::make_unique<DocLoader<0>>(kSounds, "sounds:"));
    write(f.dir, "a.sounds.json", "value: x");
    write(f.dir, "b.JSON", "value: x");
    write(f.dir, "c.json", "value: x");
    CHECK(f.s().get(f.s().load<Node>("a.sounds.json"))->value == "sounds:x");
    const AssetId<Node> b = f.s().load<Node>("b.JSON");
    CHECK(f.s().get(b)->value == "json:x");
    f.s().register_loader<Node>(std::make_unique<DocLoader<0>>(kJson, "json2:"));
    CHECK(f.s().get(f.s().load<Node>("c.json"))->value == "json2:x");
    CHECK(f.s().get(b)->value == "json:x"); // loaded values keep their loader
    CHECK(f.s().get(f.s().load<Node>("a.sounds.json"))->value == "sounds:x");
}

TEST_CASE("sync: dependencies load depth first and finalize before their parent") {
    Fixture f;
    write(f.dir, "p.node", "value: P\nnode: c1.node\nnode: sub/c2.node");
    write(f.dir, "c1.node", "value: C1\nnode: g.node");
    write(f.dir, "sub/c2.node", "value: C2\nnode: ../g.node");
    write(f.dir, "g.node", "value: G");
    const AssetId<Node> p = f.s().load<Node>("p.node");
    REQUIRE(f.s().state(p) == LoadState::Ready);
    const Node &pn = *f.s().get(p);
    REQUIRE(pn.deps.size() == 2);
    const auto c1 = AssetId<Node>::from_bits(pn.deps[0]);
    const auto c2 = AssetId<Node>::from_bits(pn.deps[1]);
    CHECK(c1.index == 2);
    CHECK(c2.index == 3);
    CHECK(f.s().path_of(c2) == "sub/c2.node");
    const auto g = AssetId<Node>::from_bits(f.s().get(c1)->deps[0]);
    CHECK(g.index == 4);
    CHECK(f.s().get(c2)->deps[0] == g.bits()); // shared dependency, one asset
    CHECK(g_journal.with('F') == std::vector<std::string>{"g.node", "c1.node", "sub/c2.node", "p.node"});
    const auto ev = events<Node>(f.h.app());
    REQUIRE(ev.size() == 4);
    CHECK(ev[0].second == g);
    CHECK(ev[3].second == p);
    // Parents retain their dependencies: dropping the top-level reference keeps nothing else.
    f.s().end_scope(kRootScope);
    f.s().collect_garbage();
    CHECK(f.s().state(g) == LoadState::None);
    CHECK(f.s().state(p) == LoadState::None);
}

TEST_CASE("sync: a failed dependency fails the parent; unreferenced queued ones are discarded") {
    Fixture f;
    test::CaptureLog log;
    write(f.dir, "p.node", "value: P\nnode: missing.node\nnode: ok.node");
    write(f.dir, "ok.node", "value: OK");
    const int live = g_live.load();
    const AssetId<Node> p = f.s().load<Node>("p.node");
    CHECK(f.s().state(p) == LoadState::Failed);
    REQUIRE(f.s().error(p) != nullptr);
    CHECK(f.s().error(p)->message.find("dependency 'missing.node' failed") != std::string::npos);
    CHECK(f.s().pending() == 0);
    CHECK(g_journal.count('D') == 1); // p only: missing.node has no file, ok.node is discarded
    CHECK(g_live.load() == live);     // the decoded parent was destroyed
    const AssetId<Node> ok = f.s().load<Node>("ok.node");
    CHECK(f.s().state(ok) == LoadState::Ready);
}

TEST_CASE("sync: decode, finalize and missing-loader failures") {
    Fixture f;
    test::CaptureLog log;
    write(f.dir, "bad.node", "value: x\nfail: decode");
    write(f.dir, "fin.node", "value: x\nfail: finalize");
    write(f.dir, "noloader.node", "value: x\nnode: thing.zzz");
    const int live = g_live.load();
    const AssetId<Node> bad = f.s().load<Node>("bad.node");
    CHECK(f.s().error(bad)->code == ErrorCode::Parse);
    CHECK(f.s().error(bad)->message == "bad.node: bad content");
    const AssetId<Node> fin = f.s().load<Node>("fin.node");
    CHECK(f.s().state(fin) == LoadState::Failed);
    CHECK(f.s().error(fin)->message == "fin.node: finalize refused");
    const AssetId<Node> nl = f.s().load<Node>("noloader.node");
    CHECK(f.s().state(nl) == LoadState::Failed);
    CHECK(f.s().error(nl)->code == ErrorCode::Unsupported);
    CHECK(f.s().error(nl)->message.find("no loader for dependency 'thing.zzz'") != std::string::npos);
    CHECK(g_live.load() == live);
    CHECK(f.s().pending() == 0);
}

TEST_CASE("sync: dependency cycles fail every member and leave nothing pending") {
    Fixture f;
    test::CaptureLog log;
    write(f.dir, "a.node", "value: A\nnode: b.node");
    write(f.dir, "b.node", "value: B\nnode: a.node");
    write(f.dir, "self.node", "value: S\nnode: self.node");
    const AssetId<Node> a = f.s().load<Node>("a.node");
    CHECK(f.s().state(a) == LoadState::Failed);
    const AssetId<Node> b = f.s().load<Node>("b.node");
    CHECK(f.s().state(b) == LoadState::Failed);
    CHECK(f.s().error(b)->message.find("cycle") != std::string::npos);
    const AssetId<Node> self = f.s().load<Node>("self.node");
    CHECK(f.s().state(self) == LoadState::Failed);
    CHECK(f.s().pending() == 0);
    CHECK(g_journal.count('F') == 0);
}

TEST_CASE("sync: LoadContext::read reads next to the asset; a missing file fails the decode") {
    Fixture f;
    write(f.dir, "lvl/map.node", "value: M\nread: data/extra.txt");
    write(f.dir, "lvl/data/extra.txt", "EXTRA");
    write(f.dir, "lvl/broken.node", "read: nope.txt");
    CHECK(f.s().get(f.s().load<Node>("lvl/map.node"))->extra == "EXTRA");
    test::CaptureLog log;
    const AssetId<Node> broken = f.s().load<Node>("lvl/broken.node");
    CHECK(f.s().error(broken)->message.find("lvl/nope.txt") != std::string::npos);
}

TEST_CASE("sync: runtime assets: add, names, mark_modified, no reload") {
    Fixture f;
    Node n;
    n.value = "made";
    const AssetId<Node> unnamed = f.s().add<Node>(n);
    const AssetId<Node> named = f.s().add<Node>(n, "gen/thing.node");
    CHECK(f.s().state(unnamed) == LoadState::Ready);
    CHECK(f.s().path_of(unnamed) == "");
    CHECK(f.s().path_of(named) == "gen/thing.node"); // named runtime: its name
    CHECK(f.s().load<Node>("gen/./thing.node") == named); // found by name, no file needed
    CHECK(f.s().get(named)->value == "made");
    CHECK(f.s().get(named)->finalized == 0); // no finalize for runtime assets
    f.s().get_mut(named)->value = "edited";
    f.s().mark_modified(named);
    CHECK(f.s().version(named) == 2);
    const auto ev = events<Node>(f.h.app());
    CHECK(ev.back() == std::pair{Kind::Modified, named});
    Status st = f.s().reload(named);
    REQUIRE(!st);
    CHECK(st.error().code == ErrorCode::Unsupported);
    CHECK(!f.s().reload(AssetId<Node>{}));
    // A dependency on a runtime asset resolves to it.
    write(f.dir, "gen/user.node", "value: U\nnode: thing.node");
    const AssetId<Node> user = f.s().load<Node>("gen/user.node");
    CHECK(f.s().get(user)->deps[0] == named.bits());
}

TEST_CASE("sync: reload swaps the value, keeps the old one on failure, recovers a failed asset") {
    Fixture f;
    test::CaptureLog log;
    write(f.dir, "a.node", "value: one");
    const AssetId<Node> a = f.s().load<Node>("a.node");
    write(f.dir, "a.node", "value: two");
    REQUIRE(f.s().reload(a).ok());
    CHECK(f.s().get(a)->value == "two");
    CHECK(f.s().version(a) == 2);
    CHECK(g_journal.with('U') == std::vector<std::string>{"a.node"}); // old value unloaded
    CHECK(events<Node>(f.h.app()).back() == std::pair{Kind::Modified, a});
    write(f.dir, "a.node", "fail: decode");
    CHECK(!f.s().reload(a));
    CHECK(f.s().state(a) == LoadState::Ready);
    CHECK(f.s().get(a)->value == "two");
    CHECK(f.s().version(a) == 2);
    // A Failed asset loads on reload: Loaded, not Modified.
    const AssetId<Node> m = f.s().load<Node>("late.node");
    CHECK(f.s().state(m) == LoadState::Failed);
    write(f.dir, "late.node", "value: here");
    REQUIRE(f.s().reload(m).ok());
    CHECK(f.s().get(m)->value == "here");
    CHECK(events<Node>(f.h.app()).back() == std::pair{Kind::Loaded, m});
}

TEST_CASE("sync: reload re-finalizes the assets that depend on the reloaded one") {
    Fixture f;
    write(f.dir, "top.node", "value: T\nnode: mid.node");
    write(f.dir, "mid.node", "value: M\nnode: leaf.node");
    write(f.dir, "leaf.node", "value: L1");
    write(f.dir, "other.node", "value: O");
    const AssetId<Node> top = f.s().load<Node>("top.node");
    const AssetId<Node> other = f.s().load<Node>("other.node");
    const auto leaf = AssetId<Node>::from_bits(
        f.s().get(AssetId<Node>::from_bits(f.s().get(top)->deps[0]))->deps[0]);
    g_journal.clear();
    write(f.dir, "leaf.node", "value: L2");
    REQUIRE(f.s().reload(leaf).ok());
    CHECK(g_journal.with('F') == std::vector<std::string>{"leaf.node", "mid.node", "top.node"});
    CHECK(f.s().version(top) == 2);
    CHECK(f.s().version(other) == 1);
    CHECK(f.s().get(leaf)->value == "L2");
    CHECK(f.s().version(AssetId<Node>::from_bits(f.s().get(top)->deps[0])) == 2);
}

TEST_CASE("sync: TextAsset loader and the AssetResolver face") {
    Fixture f;
    write(f.dir, "notes/readme.txt", "hello");
    const AssetId<TextAsset> t = f.s().load<TextAsset>("notes/readme.txt");
    CHECK(f.s().get(t)->text == "hello");
    AssetResolver &r = *f.h.app().asset_resolver();
    const std::uint64_t bits = r.resolve(type_id<TextAsset>(), "notes/readme.txt");
    CHECK(bits == t.bits());
    CHECK(r.path_of(type_id<TextAsset>(), bits) == "notes/readme.txt");
    CHECK(r.read_text("notes/readme.txt").value() == "hello");
    CHECK(!r.read_text("nope.txt"));
    CHECK(r.pending() == 0);
    CHECK(r.resolve(type_id<TextAsset>(), "x.unknown") == 0);
}

TEST_CASE("sync: load_now returns the id or the asset's error") {
    Fixture f;
    write(f.dir, "a.node", "value: A\nnode: b.node");
    write(f.dir, "b.node", "value: B");
    Result<AssetId<Node>> a = f.s().load_now<Node>("a.node");
    REQUIRE(a);
    CHECK(f.s().get(*a)->value == "A");
    test::CaptureLog log;
    write(f.dir, "bad.node", "fail: decode");
    Result<AssetId<Node>> bad = f.s().load_now<Node>("bad.node");
    REQUIRE(!bad);
    CHECK(bad.error().code == ErrorCode::Parse);
}

TEST_CASE("sync: event channels are registered on first use of a type") {
    Fixture f;
    struct Fresh {
        int v = 0;
    };
    CHECK(f.h.app().events().try_get<AssetEvent<Fresh>>() == nullptr);
    const AssetId<Fresh> id = f.s().add<Fresh>(Fresh{3});
    REQUIRE(f.h.app().events().try_get<AssetEvent<Fresh>>() != nullptr);
    CHECK(events<Fresh>(f.h.app()).size() == 1);
    CHECK(f.s().get(id)->v == 3);
    CHECK(f.s().get(AssetId<Fresh>{}) == nullptr);
    CHECK(f.s().state(AssetId<Fresh>{99, 1}) == LoadState::None);
}

namespace {

// One run of a fixed load sequence; returns ids, the finalize journal and the event order.
std::vector<std::string> run_sequence(bool async) {
    test::TempDir dir;
    write(dir, "a.root", "value: A\nmid: m1.mid\nmid: m2.mid");
    write(dir, "b.root", "value: B\nmid: m2.mid\nmid: m3.mid");
    write(dir, "m1.mid", "value: M1\nleaf: l1.leaf\nsleep: 3");
    write(dir, "m2.mid", "value: M2\nleaf: l1.leaf\nleaf: l2.leaf");
    write(dir, "m3.mid", "value: M3\nleaf: l3.leaf\nsleep: 2");
    write(dir, "l1.leaf", "value: L1\nsleep: 2");
    write(dir, "l2.leaf", "value: L2");
    write(dir, "l3.leaf", "value: L3\nsleep: 1");
    write(dir, "c.root", "value: C\nmid: m3.mid");
    g_journal.clear();
    std::unique_ptr<test::HeadlessApp> sync_app;
    std::unique_ptr<App> async_app;
    App *app = nullptr;
    if (async) {
        async_app = make_async_app();
        app = async_app.get();
    } else {
        sync_app = std::make_unique<test::HeadlessApp>();
        app = &sync_app->app();
    }
    app->add_plugin<AssetsPlugin>(config_for(dir));
    AssetServer &s = app->res<AssetServer>();
    add_doc_loaders(s);
    CHECK(s.synchronous() == !async);
    const AssetId<RootDoc> a = s.load<RootDoc>("a.root");
    const AssetId<RootDoc> b = s.load<RootDoc>("b.root");
    const AssetId<RootDoc> c = s.load<RootDoc>("c.root");
    s.wait_all();
    s.update();
    std::vector<std::string> out;
    for (const AssetId<RootDoc> r : {a, b, c}) {
        const RootDoc *doc = s.get(r);
        REQUIRE(doc != nullptr);
        std::string line = "root " + std::to_string(r.index) + ":" + doc->value;
        for (std::uint64_t d : doc->deps) {
            const auto mid = AssetId<MidDoc>::from_bits(d);
            line += " mid" + std::to_string(mid.index) + "=" + s.get(mid)->value;
            for (std::uint64_t l : s.get(mid)->deps) {
                const auto leaf = AssetId<LeafDoc>::from_bits(l);
                line += " leaf" + std::to_string(leaf.index) + "=" + s.get(leaf)->value;
            }
        }
        out.push_back(line);
    }
    for (const std::string &l : g_journal.with('F'))
        out.push_back("F " + l);
    for (const auto &[kind, id] : events<MidDoc>(*app))
        out.push_back("E mid " + std::to_string(int(kind)) + " " + std::to_string(id.index));
    for (const auto &[kind, id] : events<LeafDoc>(*app))
        out.push_back("E leaf " + std::to_string(int(kind)) + " " + std::to_string(id.index));
    return out;
}

} // namespace

TEST_CASE("determinism: the same load sequence gives the same ids, values and finalize order") {
    const std::vector<std::string> first = run_sequence(false);
    const std::vector<std::string> second = run_sequence(false);
    CHECK(first == second);
    CHECK(first.size() > 10);
}

TEST_CASE("equivalence: asynchronous mode matches synchronous ids, values and finalize order") {
    const std::vector<std::string> sync = run_sequence(false);
    for (int run = 0; run < 3; ++run) {
        const std::vector<std::string> async = run_sequence(true);
        CHECK(async == sync);
    }
}

namespace {

// "value(dep value, ...)" for a Ready asset of any Doc kind, recursively; "<state>" otherwise.
template <class T> std::string describe(AssetServer &s, AssetId<T> id);
template <class T, class U> std::string describe_deps(AssetServer &s, const T &doc) {
    std::string out;
    for (std::uint64_t bits : doc.deps) {
        out += out.empty() ? "(" : ", ";
        out += describe(s, AssetId<U>::from_bits(bits));
    }
    return out.empty() ? out : out + ")";
}
template <class T> std::string describe(AssetServer &s, AssetId<T> id) {
    const T *doc = s.get(id);
    if (doc == nullptr) return std::string(s.path_of(id)) + "<" + std::to_string(int(s.state(id))) + ">";
    if constexpr (std::is_same_v<T, RootDoc>) return doc->value + describe_deps<T, MidDoc>(s, *doc);
    if constexpr (std::is_same_v<T, MidDoc>) return doc->value + describe_deps<T, LeafDoc>(s, *doc);
    if constexpr (std::is_same_v<T, LeafDoc>) return doc->value + describe_deps<T, Node>(s, *doc);
    return doc->value;
}
template <class T> void add_events(std::vector<std::string> &out, App &app, AssetServer &s,
                                   std::string_view tag) {
    for (const auto &[kind, id] : events<T>(app))
        out.push_back("E " + std::string(tag) + " " + std::to_string(int(kind)) + " " +
                      std::string(s.path_of(id)));
}

// Four levels of shared dependencies, a failing branch and decode sleeps that reorder the worker.
// Compared by path, since async dependency ids follow decode order.
std::vector<std::string> run_deep(bool async) {
    test::TempDir dir;
    write(dir, "top1.root", "value: T1\nmid: m1.mid\nmid: m2.mid");
    write(dir, "top2.root", "value: T2\nmid: m2.mid\nmid: m3.mid\nsleep: 2");
    write(dir, "top3.root", "value: T3\nmid: bad.mid\nmid: m4.mid");
    write(dir, "top4.root", "value: T4\nmid: m4.mid");
    write(dir, "m1.mid", "value: M1\nleaf: l1.leaf\nleaf: l2.leaf\nsleep: 2");
    write(dir, "m2.mid", "value: M2\nleaf: l2.leaf\nleaf: l3.leaf");
    write(dir, "m3.mid", "value: M3\nleaf: l4.leaf\nsleep: 1");
    write(dir, "m4.mid", "value: M4\nleaf: l5.leaf");
    write(dir, "bad.mid", "value: BAD\nleaf: nope.leaf\nsleep: 1");
    write(dir, "l1.leaf", "value: L1\nnode: n1.node");
    write(dir, "l2.leaf", "value: L2\nnode: n1.node\nnode: n2.node\nsleep: 1");
    write(dir, "l3.leaf", "value: L3\nnode: n3.node");
    write(dir, "l4.leaf", "value: L4");
    write(dir, "l5.leaf", "value: L5\nnode: n4.node\nsleep: 1");
    for (int i = 1; i <= 4; ++i)
        write(dir, "n" + std::to_string(i) + ".node", "value: N" + std::to_string(i));
    g_journal.clear();
    std::unique_ptr<test::HeadlessApp> sync_app;
    std::unique_ptr<App> async_app;
    App *app = nullptr;
    if (async) {
        async_app = make_async_app();
        app = async_app.get();
    } else {
        sync_app = std::make_unique<test::HeadlessApp>();
        app = &sync_app->app();
    }
    app->add_plugin<AssetsPlugin>(config_for(dir));
    AssetServer &s = app->res<AssetServer>();
    add_doc_loaders(s);
    std::vector<AssetId<RootDoc>> tops;
    for (int i = 1; i <= 4; ++i) {
        tops.push_back(s.load<RootDoc>("top" + std::to_string(i) + ".root"));
        if (async && i == 2) s.update(); // some progress between loads
    }
    s.wait_all();
    s.update();
    std::vector<std::string> out;
    for (const AssetId<RootDoc> top : tops)
        out.push_back("root " + std::to_string(top.index) + " " + describe(s, top));
    for (const std::string &l : g_journal.with('F'))
        out.push_back("F " + l);
    add_events<RootDoc>(out, *app, s, "root");
    add_events<MidDoc>(out, *app, s, "mid");
    add_events<LeafDoc>(out, *app, s, "leaf");
    add_events<Node>(out, *app, s, "node");
    out.push_back("pending " + std::to_string(s.pending()));
    return out;
}

} // namespace

TEST_CASE("equivalence: deep trees with a failing branch finalize alike in both modes") {
    test::CaptureLog log; // the failing branch logs errors
    const std::vector<std::string> sync = run_deep(false);
    CHECK(sync.front() == "root 1 T1(M1(L1(N1), L2(N1, N2)), M2(L2(N1, N2), L3(N3)))");
    CHECK(std::find(sync.begin(), sync.end(), "root 3 top3.root<4>") != sync.end());
    CHECK(sync.back() == "pending 0");
    for (int run = 0; run < 3; ++run) {
        CAPTURE(run);
        CHECK(run_deep(true) == sync);
    }
}
