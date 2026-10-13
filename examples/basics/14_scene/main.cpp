// scene_demo: the scene stack and saves, driven by a scripted player so it runs unattended.
//   title --fade--> play --push--> pause overlay (saves the play world to slot 1) --pop(msg)-->
//   play --push--> pause overlay (loads slot 1 back, checks the state hash) --pop--> play
//   --reset+fade--> title --> quit
// Every scene owns a World; scripts are tick systems added with Scene::add_system, so the whole run
// is deterministic (operations issued in tick systems apply at the next tick boundary).
//   scene_demo [--headless] [--frames N] [--hidden]        exit code 0 = save/load round trip ok
#include <cstdio>
#include <memory>
#include <string>

#include <lam/core/app.h>
#include <lam/core/default_plugins.h>
#include <lam/core/fs.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/render/render_queue.h>
#include <lam/render/sprite.h>
#include <lam/scene/save.h>
#include <lam/scene/scene.h>

namespace demo {
struct Ball {                                                   // simulation state: saved and hashed
    lam::Vec2 vel;
    std::int32_t bounces = 0;
};
struct Score {                                                  // world resource: saved and hashed
    std::int32_t bounces = 0;
};
struct Script {                                                 // per-scene world resource, not saved
    std::int32_t ticks = 0;                                     // (a load must not rewind the script)
};
} // namespace demo
LAM_REFLECT(demo::Ball, vel, bounces)
LAM_REFLECT(demo::Score, bounces)

namespace {
using namespace lam;
using namespace lam::literals;

constexpr Vec2 kField{320, 180};
constexpr Transition kFade{.kind = TransitionKind::Fade, .ticks = 12, .color = kBlack};

struct DemoState {                                              // App resource
    std::uint64_t saved_hash = 0;
    std::uint64_t saved_tick = 0;
    bool loaded_ok = false;
    std::string save_dir;
};

int tick_script(Ctx &ctx) {
    return ++ctx.local<demo::Script>().ticks;
}

void fill(Ctx &ctx, Color c) {                                  // full-screen background
    if (RenderQueue *q = ctx.try_res<RenderQueue>()) q->rect({0, 0, kField.x, kField.y}, c, kLayerScreenFx);
}

// ---- scenes ------------------------------------------------------------------------------------

class PlayScene;
class PauseScene;

class TitleScene final : public Scene {
public:
    explicit TitleScene(bool last) : last_(last) {}
    std::string_view name() const override { return "title"; }
    void enter(App &app, World &w) override {
        std::printf("[tick %3llu] title: enter%s\n", static_cast<unsigned long long>(app.tick()),
                    last_ ? " (the end)" : "");
        w.resources().insert<demo::Script>();
        add_system(app, Stage::Simulate, last_ ? &TitleScene::finish : &TitleScene::script, "demo.title");
        add_system(app, Stage::Render, &TitleScene::draw, "demo.title.draw");
    }
    void exit(App &app, World &) override {
        std::printf("[tick %3llu] title: exit\n", static_cast<unsigned long long>(app.tick()));
    }

private:
    static void script(Ctx &ctx);
    static void finish(Ctx &ctx) {
        if (tick_script(ctx) != 30) return;
        const DemoState &d = ctx.res<DemoState>();
        std::printf("done: save/load %s\n", d.loaded_ok ? "ok" : "FAILED");
        ctx.app().quit(d.loaded_ok ? 0 : 1);
    }
    static void draw(Ctx &ctx) { fill(ctx, Color{30, 40, 90, 255}); }
    bool last_;
};

class PlayScene final : public Scene {
public:
    std::string_view name() const override { return "play"; }
    void enter(App &app, World &w) override {
        std::printf("[tick %3llu] play: enter, spawning balls\n", static_cast<unsigned long long>(app.tick()));
        w.resources().insert<demo::Script>();
        w.resources().insert<demo::Score>();
        w.snapshot_resource<demo::Score>();                     // part of saves and the hash
        Rng &rng = w.rng("demo.balls"_id);
        for (int i = 0; i < 24; ++i) {
            const Entity e = w.create();
            w.add<Transform>(e, Transform{{rng.unit() * kField.x, rng.unit() * kField.y}});
            w.add<demo::Ball>(e, demo::Ball{{rng.unit() * 4 - 2, rng.unit() * 4 - 2}});
        }
        add_system(app, Stage::Simulate, &PlayScene::move, "demo.play.move");
        add_system(app, Stage::Simulate, &PlayScene::script, "demo.play.script");
        add_system(app, Stage::Render, &PlayScene::draw, "demo.play.draw");
    }
    void exit(App &app, World &) override {
        std::printf("[tick %3llu] play: exit\n", static_cast<unsigned long long>(app.tick()));
    }
    void covered(App &app) override {
        std::printf("[tick %3llu] play: covered (time stops)\n", static_cast<unsigned long long>(app.tick()));
    }
    void resumed(App &app, const SceneMessage *msg) override {
        std::printf("[tick %3llu] play: resumed with message %lld\n",
                    static_cast<unsigned long long>(app.tick()), msg ? static_cast<long long>(msg->value) : -1LL);
    }

private:
    static void move(Ctx &ctx) {
        demo::Score &score = ctx.local<demo::Score>();
        for (auto [e, t, b] : ctx.world().view<Transform, demo::Ball>()) {
            t.position += b.vel;
            const int before = b.bounces;
            if (t.position.x < 0 || t.position.x > kField.x) b.vel.x = -b.vel.x, ++b.bounces;
            if (t.position.y < 0 || t.position.y > kField.y) b.vel.y = -b.vel.y, ++b.bounces;
            score.bounces += b.bounces - before;
        }
    }
    static void script(Ctx &ctx);
    static void draw(Ctx &ctx) {
        fill(ctx, Color{20, 70, 40, 255});
        RenderQueue *q = ctx.try_res<RenderQueue>();
        if (!q) return;
        for (auto [e, t, b] : ctx.world().view<Transform, demo::Ball>())
            q->rect({t.position.x - 2, t.position.y - 2, 4, 4}, kWhite, kLayerScreenFx + 1);
    }
};

class PauseScene final : public Scene {
public:
    explicit PauseScene(bool load) : load_(load) {}
    std::string_view name() const override { return "pause"; }
    bool overlay() const override { return true; }              // the play world stays visible
    void enter(App &app, World &w) override {
        std::printf("[tick %3llu] pause: enter (%s)\n", static_cast<unsigned long long>(app.tick()),
                    load_ ? "load slot 1" : "save slot 1");
        w.resources().insert<demo::Script>();
        add_system(app, Stage::Simulate, &PauseScene::script, "demo.pause.script");
        add_system(app, Stage::Render, &PauseScene::draw, "demo.pause.draw");
        SceneStack &stack = app.res<SceneStack>();
        World &play = stack.at(stack.size() - 2)->world();     // the scene below
        DemoState &d = app.res<DemoState>();
        SaveSlots slots(d.save_dir, "scene_demo", 1);
        if (!load_) {
            Result<std::string> data = serialize_world_text(play, app.serialize_context());
            const std::string summary = "{\"tick\":" + std::to_string(play.tick()) + "}";
            Status s = data ? slots.save(1, SaveEnvelope{.summary_json = summary, .data_json = *data})
                            : Status(data.error());
            d.saved_hash = play.state_hash();
            d.saved_tick = play.tick();
            std::printf("           saved play world (tick %llu, hash %016llx): %s\n",
                        static_cast<unsigned long long>(d.saved_tick),
                        static_cast<unsigned long long>(d.saved_hash), s ? "ok" : s.error().message.c_str());
            return;
        }
        std::printf("           play world now at tick %llu, hash %016llx\n",
                    static_cast<unsigned long long>(play.tick()),
                    static_cast<unsigned long long>(play.state_hash()));
        Result<SaveEnvelope> env = slots.load(1);
        Status s = env ? deserialize_world_text(play, env->data_json, app.serialize_context())
                       : Status(env.error());
        d.loaded_ok = s && play.state_hash() == d.saved_hash && play.tick() == d.saved_tick;
        std::printf("           loaded slot 1 %s: tick %llu, hash %016llx (%s)\n",
                    s ? "ok" : s.error().message.c_str(), static_cast<unsigned long long>(play.tick()),
                    static_cast<unsigned long long>(play.state_hash()),
                    d.loaded_ok ? "matches the save" : "MISMATCH");
    }
    void exit(App &app, World &) override {
        std::printf("[tick %3llu] pause: exit\n", static_cast<unsigned long long>(app.tick()));
    }

private:
    static void script(Ctx &ctx) {
        if (tick_script(ctx) == 20) ctx.res<SceneStack>().pop(SceneMessage{"resume"_id, 1, StrId{}});
    }
    static void draw(Ctx &ctx) {
        if (RenderQueue *q = ctx.try_res<RenderQueue>())
            q->rect({100, 60, 120, 60}, Color{0, 0, 0, 180}, kLayerUi);
    }
    bool load_;
};

void TitleScene::script(Ctx &ctx) {
    if (tick_script(ctx) == 30) ctx.res<SceneStack>().replace(std::make_unique<PlayScene>(), kFade);
}

void PlayScene::script(Ctx &ctx) {
    const int t = tick_script(ctx);                             // play ticks only (not while paused)
    SceneStack &stack = ctx.res<SceneStack>();
    if (t == 90) stack.push(std::make_unique<PauseScene>(false));
    if (t == 150) stack.push(std::make_unique<PauseScene>(true));
    if (t == 180) stack.reset(std::make_unique<TitleScene>(true), kFade);
}

// The script finishes in about 340 ticks; anything far beyond that is a bug.
void watchdog(Ctx &ctx) {
    if (ctx.app().tick() < 2000 || ctx.app().quitting()) return;
    std::printf("scene_demo: script did not finish\n");
    ctx.app().quit(1);
}

} // namespace

int main(int argc, char **argv) {
    App app({.title = "scene_demo", .virtual_size = {320, 180}, .settings_file = ""}, argc, argv);
    app.add_plugins(DefaultPlugins{});
    DemoState &d = app.insert_resource<DemoState>();
    Result<std::string> exe = executable_dir();
    d.save_dir = join_path(exe ? *exe : std::string("."), "scene_demo_saves");
    app.add_system({.stage = Stage::TickEnd, .fn = &watchdog, .name = "demo.watchdog",
                    .scope = SystemScope::Once});
    app.res<SceneStack>().push(std::make_unique<TitleScene>(false));
    return app.run();
}
