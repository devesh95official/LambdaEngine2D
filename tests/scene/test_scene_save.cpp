// Scenes and saves together: a scene world saved midway and loaded by enter() in a fresh App must
// reach the same state hash as the continuous run. Also covers the scene schedule and shutdown.
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/scene/save.h>
#include <lam/scene/scene.h>
#include <lam/test/support.h>

namespace sst {
struct Bug {
    lam::Vec2 vel;
    std::int32_t hits = 0;
    lam::Entity chasing;
};
struct Hive {                                                   // world resource: saved and hashed
    std::int64_t born = 0;
    std::int64_t hits = 0;
};
struct Seen {                                                   // App resource (FrameEnd counter)
    int frame_end = 0;
};
} // namespace sst
LAM_REFLECT(sst::Bug, vel, hits, chasing)
LAM_REFLECT(sst::Hive, born, hits)

using namespace lam;
using namespace lam::literals;

namespace {

constexpr float kArena = 100.0f;

// Moves and bounces bugs, spawns and destroys through commands, draws from a world RNG stream.
void bug_sim(Ctx &ctx) {
    World &w = ctx.world();
    sst::Hive &hive = ctx.local<sst::Hive>();
    Rng &rng = ctx.rng("sst.sim"_id);
    Entity veteran;
    for (auto [e, t, b] : w.view<Transform, sst::Bug>()) {
        t.position += b.vel;
        if (t.position.x < 0 || t.position.x > kArena) {
            b.vel.x = -b.vel.x;
            ++b.hits;
            ++hive.hits;
        }
        if (t.position.y < 0 || t.position.y > kArena) {
            b.vel.y = -b.vel.y;
            ++b.hits;
            ++hive.hits;
        }
        if (!veteran && b.hits > 3) veteran = e;
    }
    if (rng.below(5) == 0) {
        const Entity e = ctx.commands().spawn();
        ctx.commands().add(e, Transform{{rng.unit() * kArena, rng.unit() * kArena}});
        ctx.commands().add(e, sst::Bug{{rng.unit() * 3 - 1.5f, rng.unit() * 3 - 1.5f}, 0, veteran});
        ++hive.born;
    }
    if (veteran && w.count<sst::Bug>() > 10 && rng.below(3) == 0) ctx.commands().destroy(veteran);
}

// The level: builds a fresh population, then (when given save data) replaces it with the save.
class LevelScene final : public Scene {
public:
    explicit LevelScene(std::string save = {}) : save_(std::move(save)) {}
    std::string_view name() const override { return "level"; }
    void enter(App &app, World &w) override {
        w.resources().insert<sst::Hive>();
        w.snapshot_resource<sst::Hive>();
        add_system(app, Stage::Simulate, bug_sim, "sst.bug_sim");
        Rng &rng = w.rng("sst.setup"_id);
        for (int i = 0; i < 8; ++i) {
            const Entity e = w.create();
            w.add<Transform>(e, Transform{{rng.unit() * kArena, rng.unit() * kArena}});
            w.add<sst::Bug>(e, sst::Bug{{rng.unit() * 4 - 2, rng.unit() * 4 - 2}, 0, Entity{}});
        }
        if (!save_.empty()) load_status = deserialize_world_text(w, save_, app.serialize_context());
        hash_at_enter = w.state_hash();
    }
    Status load_status;
    std::uint64_t hash_at_enter = 0;

private:
    std::string save_;
};

// One App with ScenePlugin and a LevelScene pushed (entered at the first frame).
struct Run {
    test::HeadlessApp h;
    LevelScene *scene = nullptr;
    explicit Run(std::string save = {}) {
        h.add<ScenePlugin>();
        auto s = std::make_unique<LevelScene>(std::move(save));
        scene = s.get();
        h.app().res<SceneStack>().push(std::move(s));
    }
    World &world() { return scene->world(); }
};

std::string world_text(App &app, const World &w) {
    Result<std::string> t = serialize_world_text(w, app.serialize_context());
    REQUIRE(t);
    return *t;
}

std::string replaced(std::string s, std::string_view from, std::string_view to) {
    const std::size_t at = s.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, "pattern not found: " << from);
    s.replace(at, from.size(), to);
    return s;
}

} // namespace

TEST_CASE("scene save: a scene's world saved midway and loaded in a fresh App continues") {
    constexpr int kHalf = 120;
    Run continuous;
    continuous.h.ticks(2 * kHalf);
    REQUIRE(continuous.world().tick() == 2 * kHalf);
    const std::uint64_t expected = continuous.world().state_hash();
    // Make sure the run churns: spawns, bounces and destroys through commands.
    const sst::Hive &hive = continuous.world().resources().get<sst::Hive>();
    CHECK(hive.born > 10);
    CHECK(hive.hits > 10);
    CHECK(continuous.world().count<sst::Bug>() < static_cast<std::size_t>(8 + hive.born));

    test::TempDir dir;
    SaveSlots slots(dir.path(), "sst", 1, 2);
    std::uint64_t mid_hash = 0;
    {
        Run first;
        first.h.ticks(kHalf);
        World &w = first.world();
        mid_hash = w.state_hash();
        const std::string summary = "{\"tick\":" + std::to_string(w.tick()) + "}";
        REQUIRE(slots.save(2, SaveEnvelope{.summary_json = summary,
                                           .data_json = world_text(first.h.app(), w)}));
    } // the first App is gone: nothing but the file carries the state
    CHECK(slots.latest() == 2);
    CHECK(slots.summaries()[1] == "{\"tick\":" + std::to_string(kHalf) + "}");
    const Result<SaveEnvelope> env = slots.load(2);
    REQUIRE(env);

    Run second(env->data_json);                                 // a different App, same seed
    second.h.ticks(kHalf);
    REQUIRE_MESSAGE(second.scene->load_status,
                    (second.scene->load_status ? "" : second.scene->load_status.error().message));
    CHECK(second.scene->hash_at_enter == mid_hash);
    CHECK(second.world().tick() == 2 * kHalf);
    CHECK(second.world().state_hash() == expected);
    CHECK(world_text(second.h.app(), second.world()) ==
          world_text(continuous.h.app(), continuous.world()));
}

TEST_CASE("scene save: a failed load leaves the scene's world as enter() built it") {
    Run reference;
    reference.h.frames(1);
    const std::uint64_t fresh = reference.scene->hash_at_enter;

    Run source;
    source.h.ticks(15);
    const std::string good = world_text(source.h.app(), source.world());
    struct Case {
        const char *what;
        std::string text;
    };
    const Case cases[] = {
        {"not JSON", good.substr(0, good.size() / 2)},
        {"unknown component", replaced(good, R"("component":"Bug")", R"("component":"Bugz")")},
        {"unknown field", replaced(good, R"({"vel")", R"({"speed":1,"vel")")},
        {"wrong layout", replaced(good, R"("layout":1)", R"("layout":7)")},
        {"dead entity", replaced(good, R"("free":[)", R"("free":[0,)")},
        {"unknown resource", replaced(good, R"("resource":"Hive")", R"("resource":"Hiv")")},
    };
    for (const Case &c : cases) {
        CAPTURE(c.what);
        Run r(c.text);
        r.h.frames(1);
        CHECK_FALSE(r.scene->load_status);
        CHECK(r.scene->hash_at_enter == fresh);
    }
    // The good text loads.
    Run ok(good);
    ok.h.frames(1);
    CHECK(ok.scene->load_status);
    CHECK(ok.scene->hash_at_enter == source.world().state_hash());
}

TEST_CASE("scene: engine systems run at their stage, scope and order") {
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    std::istringstream lines(h.app().schedule_text());
    std::string line;
    int found = 0;
    auto has = [&](std::string_view part) { return line.find(part) != std::string::npos; };
    while (std::getline(lines, line)) {
        if (!has("[lam.scene]")) continue;
        CAPTURE(line);
        ++found;
        if (has(" scene_apply_tick")) {
            CHECK((has("TickBegin") && has("Once") && has("-2000")));
        } else if (has(" scene_apply")) {
            CHECK((has("FrameBegin") && has("Once") && has("-2000")));
        } else if (has(" scene_fade")) {
            CHECK((has("RenderUI") && has("Once") && has("1000") && !has("-1000")));
        } else {
            FAIL("unexpected scene system");
        }
    }
    CHECK(found == 3);
}

namespace {

void count_frame_end(Ctx &ctx) {
    ++ctx.res<sst::Seen>().frame_end;
}

class FrameEndScene final : public Scene {
public:
    std::string_view name() const override { return "frame_end"; }
    void enter(App &app, World &) override {
        add_system(app, Stage::FrameEnd, count_frame_end, "sst.frame_end");
    }
};

class MenuScene final : public Scene {
public:
    std::string_view name() const override { return "menu"; }
    void enter(App &, World &) override {}
    bool overlay() const override { return true; }
};

} // namespace

TEST_CASE("scene: add_system in a Once-only stage runs while the scene's world is the Once world") {
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    App &app = h.app();
    app.insert_resource<sst::Seen>();
    SceneStack &stack = app.res<SceneStack>();
    stack.push(std::make_unique<FrameEndScene>());
    h.frames(3);
    CHECK(app.res<sst::Seen>().frame_end == 3);
    stack.push(std::make_unique<MenuScene>());                  // takes the focus (blocks input)
    h.frames(2);
    CHECK(app.res<sst::Seen>().frame_end == 3);
    stack.pop();
    h.frames(2);
    CHECK(app.res<sst::Seen>().frame_end == 5);
    stack.pop();
    h.frames(1);                                                // removed with the world
    CHECK(app.res<sst::Seen>().frame_end == 5);
}

namespace {

std::vector<std::string> g_events;

class Logged final : public Scene {
public:
    explicit Logged(std::string n) : name_(std::move(n)) {}
    ~Logged() override { g_events.push_back("delete:" + name_); }
    std::string_view name() const override { return name_; }
    void enter(App &, World &) override { g_events.push_back("enter:" + name_); }
    void exit(App &, World &) override { g_events.push_back("exit:" + name_); }

private:
    std::string name_;
};

} // namespace

TEST_CASE("scene stack: App shutdown during a fade drops queued operations (never entered)") {
    g_events.clear();
    {
        test::HeadlessApp h;
        h.add<ScenePlugin>();
        SceneStack &stack = h.app().res<SceneStack>();
        stack.push(std::make_unique<Logged>("a"));
        h.frames(1);
        stack.replace(std::make_unique<Logged>("b"),
                      Transition{.kind = TransitionKind::Fade, .ticks = 4});
        stack.push(std::make_unique<Logged>("c"));
        h.frames(2);                                            // fading out; b not entered yet
        CHECK(stack.transitioning());
        CHECK(stack.queued() == 2);
        CHECK(stack.top()->name() == "a");
    }
    std::string log;
    for (const std::string &e : g_events)
        log += e + " ";
    // a exits while its world exists; b and c are destroyed without enter/exit.
    CHECK(log.find("enter:a exit:a ") == 0);
    CHECK(log.find("enter:b") == std::string::npos);
    CHECK(log.find("exit:b") == std::string::npos);
    CHECK(log.find("delete:a") != std::string::npos);
    CHECK(log.find("delete:b") != std::string::npos);
    CHECK(log.find("delete:c") != std::string::npos);
}
