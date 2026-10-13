// SceneStack. Operations queue FIFO and apply at FrameBegin/TickBegin -2000. Each scene owns a
// World and an asset scope; fades are measured in ticks.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/debug.h>
#include <lam/core/fs.h>
#include <lam/core/platform.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include <lam/scene/scene.h>
#include <lam/test/support.h>

using namespace lam;
using namespace lam::literals;

namespace {

std::vector<std::string> g_log;

struct Ticks {                                                  // per scene world
    int n = 0;
    std::uint64_t first_app_tick = 0;
};
struct Probe {                                                  // inserted by ProbePlugin
    int alive = 1;
};

void count_ticks(Ctx &ctx) {
    Ticks &t = ctx.local<Ticks>();
    if (t.n++ == 0) t.first_app_tick = ctx.app().tick();
}

std::string world_name(const World &w) {
    return id_display(w.name());
}

// Inserts Probe into every world and removes it in shutdown_world; logs the teardown.
class ProbePlugin final : public Plugin {
public:
    std::string_view name() const override { return "test.probe"; }
    void build(App &) override {}
    void init_world(App &, World &w) override { w.resources().insert<Probe>(); }
    void shutdown_world(App &, World &w) override {
        g_log.push_back("teardown:" + world_name(w));
        w.resources().remove<Probe>();
    }
};

struct Spec {
    std::string name = "scene";
    bool overlay = false;
    bool blocks_ticks = true;
    bool blocks_input = true;
    std::string load;                                           // asset loaded in enter()
    std::function<void(App &, Scene &)> on_enter;
    std::function<void(App &, Scene &)> on_resumed;
};

class TestScene final : public Scene {
public:
    explicit TestScene(Spec s) : spec_(std::move(s)) {}
    std::string_view name() const override { return spec_.name; }
    void enter(App &app, World &w) override {
        CHECK(&w == &world());
        CHECK(entered());
        g_log.push_back("enter:" + spec_.name);
        w.resources().insert<Ticks>();
        add_system(app, Stage::Simulate, count_ticks, "test.count_ticks");
        if (!spec_.load.empty()) {
            AssetServer &assets = app.res<AssetServer>();
            CHECK(assets.current_scope() == asset_scope());
            loaded = assets.load<TextAsset>(spec_.load);
        }
        if (spec_.on_enter) spec_.on_enter(app, *this);
    }
    void exit(App &, World &w) override {
        const Probe *p = w.resources().try_get<Probe>();
        g_log.push_back("exit:" + spec_.name + (p ? ":probe" : ""));
    }
    void covered(App &) override { g_log.push_back("covered:" + spec_.name); }
    void resumed(App &app, const SceneMessage *msg) override {
        g_log.push_back("resumed:" + spec_.name + ":" + (msg ? std::to_string(msg->value) : "none"));
        if (msg) last_message = *msg;
        if (spec_.on_resumed) spec_.on_resumed(app, *this);
    }
    bool overlay() const override { return spec_.overlay; }
    bool blocks_ticks() const override { return spec_.blocks_ticks; }
    bool blocks_input() const override { return spec_.blocks_input; }
    AssetId<TextAsset> loaded;
    SceneMessage last_message;

private:
    Spec spec_;
};

std::unique_ptr<TestScene> make(std::string name, bool overlay = false, bool blocks_ticks = true,
                                bool blocks_input = true) {
    Spec s;
    s.name = std::move(name);
    s.overlay = overlay;
    s.blocks_ticks = blocks_ticks;
    s.blocks_input = blocks_input;
    return std::make_unique<TestScene>(std::move(s));
}

const WorldEntry *entry_of(const App &app, const World &w) {
    for (const WorldEntry &e : app.world_stack())
        if (e.world == &w) return &e;
    return nullptr;
}

std::string log_text() {
    std::string s;
    for (const std::string &l : g_log)
        s += l + " ";
    return s;
}

} // namespace

TEST_CASE("scene stack: operations queue and apply at the next boundary, FIFO") {
    g_log.clear();
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    App &app = h.app();
    SceneStack &stack = app.res<SceneStack>();
    stack.push(make("a"));
    stack.push(make("b"));
    CHECK(stack.size() == 0);
    CHECK(stack.queued() == 2);
    CHECK(stack.top() == nullptr);
    h.frames(1);
    CHECK(stack.queued() == 0);
    REQUIRE(stack.size() == 2);
    CHECK(stack.top()->name() == "b");
    CHECK(stack.at(0)->name() == "a");
    CHECK(log_text() == "enter:a covered:a enter:b ");
    World &wa = stack.at(0)->world();
    World &wb = stack.at(1)->world();
    CHECK(&wa != &app.main_world());
    CHECK(&wa != &wb);
    CHECK(world_name(wa) == "a");
    // b blocks a: only b takes part.
    REQUIRE(app.world_stack().size() == 1);
    CHECK(app.world_stack()[0].world == &wb);
    CHECK(app.focused_world() == &wb);
    // The scene systems ran on b only (the tick of frame 1 ran after the apply).
    CHECK(wb.resources().get<Ticks>().n == 1);
    CHECK(wa.resources().get<Ticks>().n == 0);

    g_log.clear();
    stack.replace(make("c"));
    stack.pop();
    stack.push(make("d"));
    h.frames(1);
    CHECK(log_text() == "exit:b enter:c exit:c resumed:a:none covered:a enter:d ");
    REQUIRE(stack.size() == 2);
    CHECK(stack.top()->name() == "d");

    g_log.clear();
    stack.reset(make("e"));
    h.frames(1);
    CHECK(log_text() == "exit:d exit:a enter:e ");
    CHECK(stack.size() == 1);

    g_log.clear();
    stack.reset(nullptr);
    h.frames(1);
    CHECK(log_text() == "exit:e ");
    CHECK(stack.empty());
    REQUIRE(app.world_stack().size() == 1);
    CHECK(app.world_stack()[0].world == &app.main_world());     // empty stack: {main_world}
}

TEST_CASE("scene stack: pop hands a message by value to the scene below") {
    g_log.clear();
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    SceneStack &stack = h.app().res<SceneStack>();
    stack.push(make("game"));
    stack.push(make("menu"));
    h.frames(1);
    {
        SceneMessage m{"chosen"_id, 7, "potion"_id};
        stack.pop(m);
        m.value = 99;                                           // the queued copy is unaffected
    }
    auto *game = static_cast<TestScene *>(stack.at(0));
    h.frames(1);
    CHECK(log_text().find("resumed:game:7") != std::string::npos);
    CHECK(game->last_message.kind == "chosen"_id);
    CHECK(game->last_message.value == 7);
    CHECK(game->last_message.detail == "potion"_id);
    // Popping the last scene, then once more: a warning, nothing else.
    stack.pop();
    test::CaptureLog cap;
    stack.pop();
    h.frames(1);
    CHECK(stack.empty());
    CHECK(cap.contains("empty"));
}

TEST_CASE("scene stack: one World per scene, created before enter and destroyed after exit") {
    g_log.clear();
    {
        test::HeadlessApp h;
        h.add<ProbePlugin>();                                   // before ScenePlugin
        h.add<ScenePlugin>();
        App &app = h.app();
        SceneStack &stack = app.res<SceneStack>();
        stack.push(make("a"));
        h.frames(1);
        World *wa = &stack.top()->world();
        CHECK(wa->resources().has<Probe>());                   // every plugin's init_world ran
        CHECK(wa->resources().has<DebugDraw>());
        const std::uint32_t id = wa->id();
        stack.replace(make("b"));
        h.frames(1);
        CHECK(log_text() == "enter:a exit:a:probe enter:b teardown:a ");
        CHECK(stack.top()->world().id() != id);
        for (const WorldEntry &e : app.world_stack())
            CHECK(e.world->id() != id);
        // Scene systems of the destroyed world are gone; the new world counts its own ticks.
        h.ticks(5);
        CHECK(stack.top()->world().resources().get<Ticks>().n == 6);
        stack.push(make("c", true, false, false));
        stack.push(make("d", true, false, false));
        h.frames(1);
        g_log.clear();
    } // App shutdown: every scene exits, top first, while its world resources still exist
    CHECK(log_text() == "exit:d:probe teardown:d exit:c:probe teardown:c exit:b:probe teardown:b "
                        "teardown:main ");
}

TEST_CASE("scene stack: overlay and blocking flags become world stack entries") {
    g_log.clear();
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    App &app = h.app();
    SceneStack &stack = app.res<SceneStack>();
    stack.push(make("game"));
    h.frames(1);
    World &game = stack.top()->world();
    stack.push(make("hud", true, false, false));                // overlay, blocks nothing
    h.frames(1);
    World &hud = stack.top()->world();
    REQUIRE(app.world_stack().size() == 2);
    CHECK(app.world_stack()[0].world == &game);                 // bottom first
    CHECK(app.world_stack()[1].world == &hud);
    const WorldEntry *g = entry_of(app, game);
    REQUIRE(g);
    CHECK(g->visible);
    CHECK(g->ticks);
    CHECK(g->input);
    CHECK(app.focused_world() == &hud);                         // topmost input world

    stack.push(make("pause", true, true, true));                // overlay, blocks ticks and input
    h.frames(1);
    World &pause = stack.top()->world();
    REQUIRE(app.world_stack().size() == 3);
    g = entry_of(app, game);
    const WorldEntry *u = entry_of(app, hud);
    const WorldEntry *p = entry_of(app, pause);
    REQUIRE((g && u && p));
    CHECK((g->visible && !g->ticks && !g->input));
    CHECK((u->visible && !u->ticks && !u->input));
    CHECK((p->visible && p->ticks && p->input));
    const int game_ticks = game.resources().get<Ticks>().n;
    const std::uint64_t game_tick = game.tick();
    h.ticks(10);
    CHECK(game.resources().get<Ticks>().n == game_ticks);       // time stops under the pause menu
    CHECK(game.tick() == game_tick);
    CHECK(pause.resources().get<Ticks>().n == 11);

    stack.push(make("shop"));                                   // opaque: hides everything below
    h.frames(1);
    REQUIRE(app.world_stack().size() == 1);
    CHECK(app.world_stack()[0].world == &stack.top()->world());
    stack.pop();
    stack.pop();
    h.frames(1);
    g = entry_of(app, game);
    REQUIRE(g);
    CHECK((g->visible && g->ticks && g->input));
    CHECK(stack.world_entries().size() == 2);
}

TEST_CASE("scene stack: at most kMaxWorlds worlds take part (the lowest are dropped)") {
    g_log.clear();
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    App &app = h.app();
    SceneStack &stack = app.res<SceneStack>();
    for (int i = 0; i < kMaxWorlds + 2; ++i)
        stack.push(make("layer" + std::to_string(i), true, false, false));
    test::CaptureLog cap;
    h.frames(1);
    CHECK(stack.size() == static_cast<std::size_t>(kMaxWorlds + 2));
    REQUIRE(app.world_stack().size() == static_cast<std::size_t>(kMaxWorlds));
    CHECK(app.world_stack().back().world == &stack.top()->world());
    CHECK(app.world_stack().front().world == &stack.at(2)->world());
    CHECK(cap.contains("dropped"));
}

TEST_CASE("scene stack: an App that never used scenes keeps its own world stack") {
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    App &app = h.app();
    World &extra = app.create_world("extra");
    const WorldEntry entries[] = {{&app.main_world()}, {&extra}};
    app.set_world_stack(entries);
    h.frames(3);
    CHECK(app.world_stack().size() == 2);
}

TEST_CASE("scene stack: each scene loads into its own asset scope, released after exit") {
    g_log.clear();
    test::TempDir dir;
    REQUIRE(write_file_atomic(dir.file("level.txt"), "L"));
    REQUIRE(write_file_atomic(dir.file("shared.txt"), "S"));
    REQUIRE(write_file_atomic(dir.file("root.txt"), "R"));
    test::HeadlessApp h;
    h.add<AssetsPlugin>(AssetsConfig{.root = dir.path()});
    h.add<ScenePlugin>();
    App &app = h.app();
    AssetServer &assets = app.res<AssetServer>();
    const AssetId<TextAsset> root_asset = assets.load<TextAsset>("root.txt");
    SceneStack &stack = app.res<SceneStack>();
    Spec level;
    level.name = "level";
    level.load = "level.txt";
    static AssetId<TextAsset> shared_id;
    shared_id = {};
    level.on_enter = [](App &a, Scene &s) {
        shared_id = a.res<AssetServer>().load<TextAsset>("shared.txt");
        CHECK(s.asset_scope() != kRootScope);
    };
    stack.push(std::make_unique<TestScene>(level));
    h.frames(1);
    auto *first = static_cast<TestScene *>(stack.top());
    const AssetId<TextAsset> level_id = first->loaded;
    CHECK(assets.current_scope() == kRootScope);                // restored after enter
    REQUIRE(assets.get(level_id) != nullptr);
    REQUIRE(assets.get(shared_id) != nullptr);
    // Replace with a scene that also loads shared.txt: the old scope ends only after the new scene
    // entered, so shared.txt is never unloaded in between (same id, never reloaded).
    Spec next;
    next.name = "next";
    next.load = "shared.txt";
    stack.replace(std::make_unique<TestScene>(next));
    h.frames(2);
    CHECK(static_cast<TestScene *>(stack.top())->loaded == shared_id);
    CHECK(assets.version(shared_id) == assets.version(static_cast<TestScene *>(stack.top())->loaded));
    CHECK(assets.get(level_id) == nullptr);                     // only the old scope held it
    CHECK(assets.get(shared_id) != nullptr);
    CHECK(assets.get(root_asset) != nullptr);
    stack.pop();
    h.frames(2);
    CHECK(assets.get(shared_id) == nullptr);                    // its last scope ended
    CHECK(assets.get(root_asset) != nullptr);                   // the root scope still holds it
}

TEST_CASE("scene stack: fades are measured in ticks and a second transition waits") {
    g_log.clear();
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    App &app = h.app();
    SceneStack &stack = app.res<SceneStack>();
    stack.push(make("a"));
    h.frames(1);
    const Transition fade{.kind = TransitionKind::Fade, .ticks = 4, .color = Color{10, 20, 30, 200}};
    stack.replace(make("b"), fade);
    std::vector<float> alphas;
    std::vector<bool> inputs;
    for (int f = 0; f < 8; ++f) {
        if (f == 1) stack.pop(SceneMessage{}, fade);           // queued behind the running fade
        h.frames(1);
        alphas.push_back(stack.fade_alpha());
        bool any_input = false;
        for (const WorldEntry &e : app.world_stack())
            any_input = any_input || e.input;
        inputs.push_back(any_input);
        if (f == 3) CHECK(log_text() == "enter:a exit:a enter:b ");   // the swap at full cover
        if (f < 7) CHECK(stack.fade_color() == Color{10, 20, 30, 200});
        if (f < 3) CHECK(log_text() == "enter:a ");
    }
    const std::vector<float> expected{0.25f, 0.5f, 0.75f, 1.0f, 0.75f, 0.5f, 0.25f, 0.0f};
    CHECK(alphas == expected);
    // No world has input while a transition runs.
    CHECK(inputs == std::vector<bool>{false, false, false, false, false, false, false, true});
    CHECK(stack.queued() == 1);                                 // the pop still waits...
    CHECK_FALSE(stack.transitioning());
    h.frames(1);
    CHECK(stack.transitioning());                               // ...and starts now
    CHECK(stack.fade_alpha() == 0.25f);
    h.frames(3);
    CHECK(stack.empty());
    CHECK(stack.fade_alpha() == 1.0f);
    h.frames(4);
    CHECK_FALSE(stack.transitioning());
    CHECK(stack.queued() == 0);

    SUBCASE("ticks == 0 is an immediate operation") {
        stack.push(make("z"), Transition{.kind = TransitionKind::Fade, .ticks = 0});
        h.frames(1);
        CHECK(stack.size() == 1);
        CHECK_FALSE(stack.transitioning());
    }
    SUBCASE("a paused App advances the fade once per frame") {
        stack.push(make("p"), Transition{.kind = TransitionKind::Fade, .ticks = 2});
        app.set_paused(true);
        h.frames(1);
        CHECK(stack.fade_alpha() == 0.5f);
        h.frames(1);
        CHECK(stack.size() == 1);
        h.frames(2);
        CHECK_FALSE(stack.transitioning());
        app.set_paused(false);
    }
}

TEST_CASE("scene stack: scene_fade draws one overlay quad through the RenderQueue") {
    test::HeadlessApp h(AppConfig{.virtual_size = {320, 180}});
    h.add<ScenePlugin>();
    App &app = h.app();
    RenderQueue &queue = app.insert_resource<RenderQueue>(std::size_t{256});
    SceneStack &stack = app.res<SceneStack>();
    stack.push(make("a"), Transition{.kind = TransitionKind::Fade, .ticks = 2, .color = Color{255, 0, 0, 255}});
    h.frames(1);                                                // alpha 0.5
    REQUIRE(queue.size() == 1);
    const DrawQuad &q = queue.quads()[0];
    CHECK(key_layer(q.key) == kLayerOverlay);
    CHECK(q.v[0].color == Color{255, 0, 0, 128});
    CHECK(q.v[0].pos == Vec2{0, 0});
    CHECK(q.v[2].pos == Vec2{320, 180});
    queue.clear();
    h.frames(3);                                                // 1.0, 0.5, then done (0)
    CHECK(queue.size() == 2);
    queue.clear();
    h.frames(2);
    CHECK(queue.size() == 0);                                   // nothing drawn when idle
}

namespace {

// A Simulate system on the "a" world: at its 6th tick it pushes "b" (a tick-stage operation).
void push_at_sixth(Ctx &ctx) {
    if (ctx.local<Ticks>().n != 6) return;
    Spec s;
    s.name = "b";
    ctx.res<SceneStack>().push(std::make_unique<TestScene>(s),
                               Transition{.kind = TransitionKind::Fade, .ticks = 3});
}

// Per app tick: {app tick, a's world tick, b's world tick, a's hash, fade alpha * 1000}.
std::vector<std::array<std::uint64_t, 5>> g_trace;

void trace_tick(Ctx &ctx) {
    const SceneStack &stack = ctx.res<SceneStack>();
    std::array<std::uint64_t, 5> row{ctx.app().tick(), 0, 0, 0,
                                     static_cast<std::uint64_t>(stack.fade_alpha() * 1000.0f)};
    if (stack.size() >= 1) {
        row[1] = stack.at(0)->world().tick();
        row[3] = stack.at(0)->world().state_hash();
    }
    if (stack.size() >= 2) row[2] = stack.at(1)->world().tick();
    g_trace.push_back(row);
}

// Runs past app tick 36 with the NullPlatform clock giving about `ticks_per_frame` ticks per
// frame (not headless, so the fixed clock decides); returns the first 36 trace rows.
std::vector<std::array<std::uint64_t, 5>> paced_run(int ticks_per_frame) {
    g_trace.clear();
    AppConfig cfg;
    cfg.settings_file = "";
    cfg.max_ticks_per_frame = 4;
    App app(cfg);
    app.set_platform(std::make_unique<NullPlatform>(60 / ticks_per_frame));
    app.add_plugin<ScenePlugin>();
    SystemDesc d;
    d.stage = Stage::TickEnd;
    d.scope = SystemScope::Once;
    d.fn = trace_tick;
    d.name = "test.trace";
    app.add_system(d);
    Spec a;
    a.name = "a";
    a.on_enter = [](App &ap, Scene &s) { s.add_system(ap, Stage::Simulate, push_at_sixth, "test.push"); };
    app.res<SceneStack>().push(std::make_unique<TestScene>(a));
    int frames = 0;
    while (app.tick() < 36 && frames++ < 1000)
        app.run_frames(1);
    REQUIRE(g_trace.size() >= 36);
    g_trace.resize(36);
    if (ticks_per_frame > 1) CHECK(frames < 30);                // several ticks per frame ran
    return g_trace;
}

} // namespace

TEST_CASE("scene stack: operations from tick systems take effect at the next tick boundary") {
    g_log.clear();
    const auto one = paced_run(1);
    const auto three = paced_run(3);
    CHECK(one == three);                                        // same ticks, fades and hashes
    // "a" ticks from app tick 1; its 6th tick queues the push; the 3-tick fade-out runs on ticks
    // 7..9 and "b" enters at the TickBegin of tick 9, which "a" no longer runs.
    CHECK(one[7][4] == 666);                                    // tick 8: fade 2/3
    CHECK(one[7][1] == 8);
    CHECK(one[8][1] == 8);                                      // a held from tick 9
    CHECK(one[8][2] == 1);                                      // b's first tick is app tick 9
    CHECK(one[35][1] + one[35][2] == 36);
}

TEST_CASE("scene stack: operations queued by scene callbacks run in the same apply") {
    g_log.clear();
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    SceneStack &stack = h.app().res<SceneStack>();
    Spec boot;
    boot.name = "boot";
    boot.on_enter = [](App &app, Scene &) { app.res<SceneStack>().replace(make("title")); };
    stack.push(std::make_unique<TestScene>(boot));
    h.frames(1);
    CHECK(log_text() == "enter:boot exit:boot enter:title ");
    REQUIRE(stack.size() == 1);
    CHECK(stack.top()->name() == "title");
}

TEST_CASE("scene stack: wait_for_assets holds the new world until its loads are Ready") {
    // Needs the asynchronous AssetServer (pending() > 0 between frames).
    g_log.clear();
    test::TempDir dir;
    REQUIRE(write_file_atomic(dir.file("big.txt"), std::string(1 << 20, 'x')));
    AppConfig cfg;
    cfg.settings_file = "";
    App app(cfg);
    app.add_plugin<AssetsPlugin>(AssetsConfig{.root = dir.path()});
    app.add_plugin<ScenePlugin>();
    Spec s;
    s.name = "level";
    s.load = "big.txt";
    app.res<SceneStack>().push(std::make_unique<TestScene>(s));
    SceneStack &stack = app.res<SceneStack>();
    // This App runs on the real clock (no --headless): frames are unpaced, so a fixed number of
    // frames can end before the worker decodes 1 MiB or before 1/60 s has elapsed for a tick. Run
    // frames for wall time instead (2 ms apart, 10 s cap) until the scene's world has ticked.
    int ticked_while_pending = 0, pending_frames = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        app.run_frames(1);
        if (stack.empty()) continue;
        const bool pending = app.res<AssetServer>().pending() > 0;
        const int n = stack.top()->world().resources().get<Ticks>().n;
        pending_frames += pending ? 1 : 0;
        if (pending && n != 0) ++ticked_while_pending;
        if (!pending && !stack.waiting_for_assets() && n > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(pending_frames > 0);                                  // enter() loads; finalize is a frame later
    CHECK(ticked_while_pending == 0);
    CHECK_FALSE(stack.waiting_for_assets());
    CHECK(stack.top()->world().resources().get<Ticks>().n > 0);
}

namespace {

// A loader whose decode (on the asset worker) blocks until g_release_slow is set: an unrelated
// background load that stays pending for as long as a test needs.
struct SlowDoc { int value = 0; };
std::atomic<bool> g_release_slow{false};
constexpr std::string_view kSlowExt[] = {".slow"};
class SlowLoader final : public AssetLoader<SlowDoc> {
public:
    std::span<const std::string_view> extensions() const override { return kSlowExt; }
    Result<SlowDoc> decode(std::span<const std::byte>, LoadContext &) override {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!g_release_slow.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return SlowDoc{1};
    }
};

} // namespace

TEST_CASE("scene stack: wait_for_assets waits for the scene's own scope, not unrelated loads") {
    g_log.clear();
    g_release_slow = false;
    test::TempDir dir;
    REQUIRE(write_file_atomic(dir.file("bg.slow"), std::string("x")));
    AppConfig cfg;
    cfg.settings_file = "";
    App app(cfg);
    app.add_plugin<AssetsPlugin>(AssetsConfig{.root = dir.path()});
    app.add_plugin<ScenePlugin>();
    AssetServer &assets = app.res<AssetServer>();
    REQUIRE_FALSE(assets.synchronous());
    assets.register_loader<SlowDoc>(std::make_unique<SlowLoader>());
    // Past the startup tick hold, which waits for every load.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (app.tick() == 0 && std::chrono::steady_clock::now() < deadline) {
        app.run_frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(app.tick() > 0);
    const AssetId<SlowDoc> bg = assets.load<SlowDoc>("bg.slow", kRootScope); // stays pending
    SceneStack &stack = app.res<SceneStack>();
    stack.push(make("level"));                                  // loads nothing of its own
    int ticked = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        app.run_frames(1);
        if (!stack.empty() && stack.top()->world().resources().get<Ticks>().n > 0) {
            ticked = stack.top()->world().resources().get<Ticks>().n;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(ticked > 0);                                          // ticked while bg.slow was pending
    CHECK(assets.pending() > 0);
    CHECK(assets.pending(stack.top()->asset_scope()) == 0);
    CHECK_FALSE(stack.waiting_for_assets());
    g_release_slow = true;
    assets.wait_all();
    CHECK(assets.state(bg) == LoadState::Ready);
}

TEST_CASE("scene stack: steady state does not allocate") {
    test::HeadlessApp h;
    h.add<ScenePlugin>();
    SceneStack &stack = h.app().res<SceneStack>();
    stack.push(make("game"));
    stack.push(make("hud", true, false, false));
    h.ticks(120);
    CHECK_NO_ALLOC(h.ticks(600));
    CHECK(stack.size() == 2);
}

TEST_CASE("scene stack: a running fade (and its overlay quad) does not allocate") {
    test::HeadlessApp h(AppConfig{.virtual_size = {320, 180}});
    h.add<ScenePlugin>();
    App &app = h.app();
    RenderQueue &queue = app.insert_resource<RenderQueue>(std::size_t{1024});   // no render_clear here
    SceneStack &stack = app.res<SceneStack>();
    stack.push(make("game"));
    h.ticks(60);
    const Transition slow{.kind = TransitionKind::Fade, .ticks = 400};
    stack.push(make("menu", true), slow);
    h.ticks(100);                                               // warm-up inside the fade-out
    queue.clear();
    CHECK_NO_ALLOC(h.ticks(200));                               // fade-out: alpha rises, one quad a frame
    CHECK(queue.size() == 200);
    CHECK(stack.size() == 1);
    h.ticks(150);                                               // the push happens at full cover
    REQUIRE(stack.size() == 2);
    CHECK(stack.transitioning());
    queue.clear();
    CHECK_NO_ALLOC(h.ticks(300));                               // fade-in
    CHECK(queue.size() == 300);
    h.ticks(100);
    CHECK_FALSE(stack.transitioning());
}
