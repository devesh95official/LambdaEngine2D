// Asset lifetime: scopes, retains, Handle<T>, GC and slot reuse.
#include <string>
#include <utility>
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
        write(dir, "a.node", "value: A");
        write(dir, "b.node", "value: B");
        write(dir, "p.node", "value: P\nnode: c.node");
        write(dir, "c.node", "value: C");
    }
    AssetServer &s() { return *server; }
};

} // namespace

TEST_CASE("scopes: end_scope releases; the next update unloads with callbacks and Removed") {
    Fixture f;
    const AssetScope scene = f.s().begin_scope();
    CHECK(scene != kRootScope);
    const AssetId<Node> a = f.s().load<Node>("a.node", scene);
    const AssetId<Node> b = f.s().load<Node>("b.node"); // root scope
    f.s().end_scope(scene);
    CHECK(f.s().state(a) == LoadState::Ready); // GC runs at the next update()
    f.h.frames(1);
    CHECK(f.s().state(a) == LoadState::None);
    CHECK(f.s().get(a) == nullptr);
    CHECK(f.s().path_of(a) == "");
    CHECK(f.s().state(b) == LoadState::Ready);
    CHECK(g_journal.with('U') == std::vector<std::string>{"a.node"});
}

TEST_CASE("scopes: an asset held by two scopes survives ending one; one retain per scope") {
    Fixture f;
    const AssetScope s1 = f.s().begin_scope();
    const AssetScope s2 = f.s().begin_scope();
    const AssetId<Node> a = f.s().load<Node>("a.node", s1);
    CHECK(f.s().load<Node>("a.node", s1) == a); // same scope: no extra retain
    CHECK(f.s().load<Node>("a.node", s2) == a);
    f.s().end_scope(s1);
    f.s().collect_garbage();
    CHECK(f.s().state(a) == LoadState::Ready);
    f.s().end_scope(s2);
    f.s().collect_garbage();
    CHECK(f.s().state(a) == LoadState::None);
}

TEST_CASE("scopes: the current scope receives plain loads; ending it restores the root scope") {
    Fixture f;
    const AssetScope scene = f.s().begin_scope();
    f.s().set_current_scope(scene);
    CHECK(f.s().current_scope() == scene);
    const AssetId<Node> a = f.s().load<Node>("a.node");
    f.s().end_scope(scene);
    CHECK(f.s().current_scope() == kRootScope);
    f.s().collect_garbage();
    CHECK(f.s().state(a) == LoadState::None);
}

TEST_CASE("scopes: parents retain dependencies; unloading the parent releases them") {
    Fixture f;
    const AssetScope scene = f.s().begin_scope();
    const AssetId<Node> p = f.s().load<Node>("p.node", scene);
    const auto c = AssetId<Node>::from_bits(f.s().get(p)->deps[0]);
    f.s().end_scope(scene);
    f.s().collect_garbage();
    CHECK(f.s().state(p) == LoadState::None);
    CHECK(f.s().state(c) == LoadState::None);
    CHECK(g_journal.with('U') == std::vector<std::string>{"p.node", "c.node"});
    // A dependency also loaded directly outlives its parent.
    const AssetScope s2 = f.s().begin_scope();
    const AssetId<Node> p2 = f.s().load<Node>("p.node", s2);
    const AssetId<Node> c2 = f.s().load<Node>("c.node"); // root scope
    CHECK(f.s().get(p2)->deps[0] == c2.bits());
    f.s().end_scope(s2);
    f.s().collect_garbage();
    CHECK(f.s().state(p2) == LoadState::None);
    CHECK(f.s().state(c2) == LoadState::Ready);
}

TEST_CASE("scopes: Handle<T> is an explicit refcount (copy, move, assignment)") {
    Fixture f;
    const AssetScope scene = f.s().begin_scope();
    f.s().set_current_scope(scene);
    Handle<Node> h1 = f.s().load_handle<Node>("a.node");
    f.s().set_current_scope(kRootScope);
    REQUIRE(h1);
    const AssetId<Node> a = h1.id();
    f.s().end_scope(scene);
    f.s().collect_garbage();
    CHECK(f.s().state(a) == LoadState::Ready); // the handle holds it
    {
        Handle<Node> h2 = h1;            // copy: +1
        Handle<Node> h3 = std::move(h1); // move: no change; h1 empty
        CHECK(!h1);
        Handle<Node> h4;
        h4 = h2;                         // copy assignment: +1
        h4 = std::move(h3);              // move assignment releases h4's old reference
        CHECK(!h3);
        f.s().collect_garbage();
        CHECK(f.s().state(a) == LoadState::Ready);
    }
    f.s().collect_garbage();
    CHECK(f.s().state(a) == LoadState::None);
    Handle<Node> none;
    CHECK(!none);
}

TEST_CASE("scopes: a freed index is reused with a higher generation; stale ids stay dead") {
    Fixture f;
    const AssetScope scene = f.s().begin_scope();
    const AssetId<Node> a = f.s().load<Node>("a.node", scene);
    f.s().end_scope(scene);
    f.s().collect_garbage();
    const AssetId<Node> b = f.s().load<Node>("b.node");
    CHECK(b.index == a.index);
    CHECK(b.generation == a.generation + 1);
    CHECK(f.s().get(a) == nullptr);
    CHECK(f.s().state(a) == LoadState::None);
    CHECK(f.s().version(a) == 0);
    CHECK(f.s().get(b)->value == "B");
    const auto ev = events<Node>(f.h.app());
    REQUIRE(ev.size() == 3);
    CHECK(ev[1] == std::pair{Kind::Removed, a});
    CHECK(ev[2] == std::pair{Kind::Loaded, b});
}

TEST_CASE("scopes: runtime assets follow scopes too") {
    Fixture f;
    const AssetScope scene = f.s().begin_scope();
    Node n;
    n.value = "runtime";
    const AssetId<Node> r = f.s().add<Node>(n, "made.node", scene);
    CHECK(f.s().load<Node>("made.node") == r); // root scope now holds it too
    f.s().end_scope(scene);
    f.s().collect_garbage();
    CHECK(f.s().state(r) == LoadState::Ready);
    f.s().end_scope(kRootScope);
    f.s().collect_garbage();
    CHECK(f.s().state(r) == LoadState::None);
}

TEST_CASE("scopes: failed assets are collected like the others") {
    Fixture f;
    test::CaptureLog log;
    const AssetScope scene = f.s().begin_scope();
    const AssetId<Node> m = f.s().load<Node>("missing.node", scene);
    CHECK(f.s().state(m) == LoadState::Failed);
    f.s().end_scope(scene);
    f.s().collect_garbage();
    CHECK(f.s().state(m) == LoadState::None);
    CHECK(events<Node>(f.h.app()).back() == std::pair{Kind::Removed, m});
}

TEST_CASE("scopes: plugin shutdown unloads everything with callbacks; nothing leaks") {
    const int live = g_live.load();
    {
        Fixture f;
        f.s().load<Node>("a.node");
        f.s().load<Node>("p.node", f.s().begin_scope());
        f.s().add<Node>(Node{});
        CHECK(g_live.load() > live);
    }
    CHECK(g_live.load() == live);
    std::vector<std::string> unloaded = g_journal.with('U');
    std::sort(unloaded.begin(), unloaded.end());
    CHECK(unloaded == std::vector<std::string>{"a.node", "c.node", "p.node"});
}
