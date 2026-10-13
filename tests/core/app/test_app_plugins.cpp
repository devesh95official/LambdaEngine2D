// Plugin lifecycle: build, init_world, finish and shutdown order, bundles and requirements.
#include <lam/test/support.h>

#include "app_test_util.h"

using namespace lam;

namespace {

// Static so it still exists when the App destructor runs the plugins' shutdown.
std::vector<std::string> &events_log() {
    static std::vector<std::string> log;
    return log;
}

std::string joined() {
    std::string out;
    for (const auto &l : events_log()) {
        if (!out.empty()) out += ' ';
        out += l;
    }
    return out;
}

struct Marker {
    int value = 0;
};

template <char Tag>
class TagPlugin final : public Plugin {
public:
    std::string_view name() const override {
        static const char n[] = {'p', '.', Tag, '\0'};
        return n;
    }
    void build(App &) override { events_log().push_back(std::string("build:") + Tag); }
    void init_world(App &, World &w) override {
        events_log().push_back(std::string("init:") + Tag + "@" + apptest::world_label(w));
    }
    void finish(App &) override { events_log().push_back(std::string("finish:") + Tag); }
    void shutdown_world(App &, World &w) override {
        events_log().push_back(std::string("down:") + Tag + "@" + apptest::world_label(w));
    }
    void shutdown(App &) override { events_log().push_back(std::string("shutdown:") + Tag); }
};

// Adds another plugin from its own build() and inserts a world resource in init_world.
class ParentPlugin final : public Plugin {
public:
    explicit ParentPlugin(int v) : value_(v) {}
    std::string_view name() const override { return "p.parent"; }
    void build(App &app) override {
        events_log().push_back("build:parent");
        app.add_plugin<TagPlugin<'c'>>();
    }
    void init_world(App &, World &w) override { w.resources().insert<Marker>(Marker{value_}); }

private:
    int value_;
};

struct Bundle {
    void build(App &app) const {
        app.add_plugin<TagPlugin<'x'>>();
        app.add_plugin<TagPlugin<'y'>>();
    }
};

class Needy final : public Plugin {
public:
    std::string_view name() const override { return "p.needy"; }
    void build(App &app) override { app.require_plugin("p.missing", name()); }
};

} // namespace

TEST_CASE("plugins: build, init_world, finish and shutdown order") {
    events_log().clear();
    {
        test::HeadlessApp h;
        App &app = h.app();
        app.add_plugin<TagPlugin<'a'>>();
        app.add_plugin<TagPlugin<'b'>>();
        CHECK(joined() == "build:a init:a@main build:b init:b@main");
        events_log().clear();
        World &level = app.create_world("level");
        CHECK(joined() == "init:a@level init:b@level");
        events_log().clear();
        h.ticks(2); // finish runs once, before frame 1
        CHECK(joined() == "finish:a finish:b");
        events_log().clear();
        app.destroy_world(level);
        CHECK(joined() == "down:b@level down:a@level");
        events_log().clear();
        app.create_world("extra");
        events_log().clear();
    }
    CHECK(joined() == "down:b@extra down:a@extra down:b@main down:a@main shutdown:b shutdown:a");
}

TEST_CASE("plugins: has_plugin, nested add_plugin, bundles, world resources") {
    events_log().clear();
    test::HeadlessApp h;
    App &app = h.app();
    ParentPlugin &parent = app.add_plugin<ParentPlugin>(7);
    (void)parent;
    CHECK(app.has_plugin("p.parent"));
    CHECK(app.has_plugin("p.c"));
    CHECK_FALSE(app.has_plugin("p.x"));
    app.add_plugins(Bundle{});
    CHECK(app.has_plugin("p.x"));
    CHECK(app.has_plugin("p.y"));
    CHECK(app.main_world().resources().get<Marker>().value == 7);
    World &w = app.create_world("w");
    CHECK(w.resources().get<Marker>().value == 7);
    // The nested plugin finished its build (and init_world) before the parent's build returned.
    CHECK(joined().rfind("build:parent build:c init:c@main", 0) == 0);
}

TEST_CASE("plugins: a plugin added after start is finished immediately") {
    events_log().clear();
    test::HeadlessApp h;
    h.ticks(1);
    h.app().add_plugin<TagPlugin<'z'>>();
    CHECK(joined() == "build:z init:z@main finish:z");
}

TEST_CASE("plugins: require_plugin is fatal at run() when missing, fine when present") {
    std::string out;
    CHECK(apptest::dies(
        [] {
            test::HeadlessApp h;
            h.app().add_plugin<Needy>();
            h.ticks(1);
        },
        &out));
    CHECK(out.find("p.missing") != std::string::npos);
    CHECK_FALSE(apptest::dies([] {
        test::HeadlessApp h;
        h.app().require_plugin("p.a", "test");
        h.app().add_plugin<TagPlugin<'a'>>();
        h.ticks(1);
    }));
}

TEST_CASE("plugins: duplicate names fail LAM_ASSERT") {
    CHECK(apptest::dies([] {
        test::HeadlessApp h;
        h.app().add_plugin<TagPlugin<'a'>>();
        h.app().add_plugin<TagPlugin<'a'>>();
    }));
}
