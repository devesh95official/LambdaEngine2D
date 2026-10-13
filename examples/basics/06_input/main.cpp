// input_demo: lam_input in one file.
//   1. Named actions with default bindings (keys, pad buttons, sticks) in an ActionMap.
//   2. Reading them in a tick system through the latched InputFrame.
//   3. The unlatched FrameInput in a frame system (menus: repeat, rebinding by capture).
//   4. A frame-time decision carried into the simulation as an intent.
//   5. An InputDriver bot, recorded and replayed to the same hash.
//
//   ./input_demo                          window: WASD/arrows/left stick move, Space/A jumps,
//                                         R then a key adds a jump binding, G sends a gift intent
//   ./input_demo --frames 120 --hidden    the same, quits after 120 frames
//   ./input_demo --headless [--frames N]  bot run, recorded and replayed in-process (exit 1 on mismatch)
//   ./input_demo --record-input run.lamr  /  --replay run.lamr   the engine flags (InputPlugin)
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include <lam/core/app.h>
#include <lam/core/log.h>
#include <lam/core/raylib_platform.h>
#include <lam/core/settings.h>
#include <lam/ecs/component.h>
#include <lam/ecs/world.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/input/input_plugin.h>
#include <lam/input/keys.h>

struct Hero {                                                   // simulation state (hashed)
    float x = 0, y = 0;
    std::int32_t jumps = 0, gifts = 0;
};
LAM_REFLECT(Hero, x, y, jumps, gifts)

using namespace lam;
using namespace lam::literals;

namespace {

struct Controls {                                               // action ids, resolved once
    AxisAction move;
    ButtonAction jump, rebind, gift;
};

void define_controls(App &app) {
    ActionMap &map = app.res<ActionMap>();
    const Controls c{
        .move = map.axis2("move",
                          {{ActionMap::key(Key::W), ActionMap::key(Key::S), ActionMap::key(Key::A), ActionMap::key(Key::D)},
                           {ActionMap::key(Key::Up), ActionMap::key(Key::Down), ActionMap::key(Key::Left),
                            ActionMap::key(Key::Right)}},
                          {{PadAxis::LeftX, PadAxis::LeftY}}),
        .jump = map.button("jump", {ActionMap::key(Key::Space), ActionMap::pad(PadButton::South)}),
        .rebind = map.button("rebind", {ActionMap::key(Key::R)}),
        .gift = map.button("gift", {ActionMap::key(Key::G)}),
    };
    map.set_repeat(c.gift, 20, 10);                             // hold G: a gift every 10 ticks after 20
    app.insert_resource<Controls>(c);
}

void spawn(Ctx &ctx) {
    ctx.world().add<Hero>(ctx.world().create());
}

// Tick system (Stage::Input): latched input, so a press is seen by exactly one tick.
void drive(Ctx &ctx) {
    const Controls &c = ctx.res<Controls>();
    const InputFrame &in = input_frame(ctx);
    ctx.world().view<Hero>().each([&](Entity, Hero &h) {
        h.x += in.axis2(c.move).x * 2.0f;
        h.y += in.axis2(c.move).y * 2.0f;
        if (in.is_pressed(c.jump)) ++h.jumps;
        for (const Intent &i : in.intent_list())               // UI decisions arrive as intents
            if (i.kind == "gift"_id) h.gifts += static_cast<std::int32_t>(i.a);
    });
}

// Frame system (Stage::Update): menus and UI read FrameInput (works while ticks are held).
void menu(Ctx &ctx) {
    const Controls &c = ctx.res<Controls>();
    const FrameInput &fi = frame_input(ctx);
    ActionMap &map = ctx.res<ActionMap>();
    if (fi.is_pressed(c.rebind) && !map.capturing()) {
        map.begin_capture(c.jump, 0, 1);                        // the next new press becomes jump slot 1
        LAM_LOG_INFO("press a key or pad button for 'jump' (Escape cancels)");
    }
    if (fi.is_repeated(c.gift)) ctx.res<InputSystem>().queue_intent({"gift"_id, 1, 0});
    if (fi.is_pressed(c.jump)) LAM_LOG_INFO("jump (bindings: {})", map.bindings(c.jump).size());
}

// A bot: runs at TickBegin -1400 with the focused world, so it decides from state.
class Bot final : public InputDriver {
public:
    explicit Bot(Controls c) : c_(c) {}
    void fill(const Ctx &ctx, std::span<InputFrame, kMaxPlayers> frames) override {
        InputFrame &f = frames[0];
        f = InputFrame{};
        const std::uint64_t t = ctx.app().tick();
        ctx.world().view<Hero>().each([&](Entity, Hero &h) {
            const float target = (t / 120) % 2 == 0 ? 100.0f : -100.0f;   // walk back and forth
            f.axes[c_.move.id] = Vec2{h.x < target ? 1.0f : -1.0f, 0.0f};
        });
        if (t % 45 == 0) press(f, c_.jump);
        if (t % 100 == 50) f.intents[f.intent_count++] = Intent{"gift"_id, 2, 0};
    }

private:
    static void press(InputFrame &f, ButtonAction a) {
        f.pressed[a.id >> 6] |= std::uint64_t{1} << (a.id & 63u);
        f.held[a.id >> 6] |= std::uint64_t{1} << (a.id & 63u);
    }
    Controls c_;
};

std::unique_ptr<App> make_app(int argc, const char *const *argv, bool window) {
    auto app = std::make_unique<App>(AppConfig{.title = "input_demo", .audio = false}, argc, argv);
    if (window) app->add_plugin<PlatformPlugin>();
    app->add_plugin<InputPlugin>();                             // ActionMap, InputSystem, ...
    if (window) app->add_plugin<RaylibInputPlugin>();           // reads devices at FrameBegin -5000
    app->register_component<Hero>();
    define_controls(*app);
    app->add_startup(&spawn, "demo.spawn");
    app->add_system(Stage::Input, &drive, "demo.drive");
    app->add_system(Stage::Update, &menu, "demo.menu");
    return app;
}

Hero hero_of(App &app) {
    Hero out;
    app.main_world().view<Hero>().each([&](Entity, Hero &h) { out = h; });
    return out;
}

// --headless: a bot plays while InputRecorder records; a second App replays the file.
int headless(int argc, const char *const *argv) {
    const std::string path = (std::filesystem::temp_directory_path() / "lam_input_demo.lamr").string();
    auto live = make_app(argc, argv, false);
    const int frames = live->options().frames > 0 ? live->options().frames : 300;
    Bot bot(live->res<Controls>());
    InputRecorder recorder;
    if (Status s = recorder.open(path, current_replay_header(live->seed(), live->config().tick_rate, 1)); !s) {
        std::printf("input_demo: %s\n", s.error().message.c_str());
        return 1;
    }
    live->res<InputSystem>().set_driver(&bot);
    live->res<InputSystem>().set_recorder(&recorder);
    live->run_frames(frames);                                   // headless: one tick per frame
    live->res<InputSystem>().set_recorder(nullptr);
    (void)recorder.close();
    const Hero h = hero_of(*live);
    const std::uint64_t hash = live->main_world().state_hash();
    std::printf("input_demo: bot played %llu ticks: hero at (%.0f, %.0f), %d jumps, %d gifts, hash %016llx\n",
                static_cast<unsigned long long>(live->tick()), h.x, h.y, h.jumps, h.gifts,
                static_cast<unsigned long long>(hash));

    auto replay = make_app(argc, argv, false);
    auto driver = ReplayDriver::open(path);
    if (!driver) {
        std::printf("input_demo: %s\n", driver.error().message.c_str());
        return 1;
    }
    if (Status ok = replay_compatible((*driver)->header(), replay->config().tick_rate, false); !ok) {
        std::printf("input_demo: %s\n", ok.error().message.c_str());
        return 1;
    }
    replay->set_seed((*driver)->header().seed);
    replay->res<InputSystem>().set_driver(driver->get());
    replay->run_frames(frames);
    replay->res<InputSystem>().set_driver(nullptr);
    const std::uint64_t replayed = replay->main_world().state_hash();
    std::printf("input_demo: replay of %zu recorded ticks: hash %016llx (%s)\n", (*driver)->tick_count(),
                static_cast<unsigned long long>(replayed), replayed == hash ? "match" : "MISMATCH");

    // Rebinding by capture, without a device: feed raw input as the backend would.
    ActionMap &map = replay->res<ActionMap>();
    const Controls &c = replay->res<Controls>();
    map.begin_capture(c.jump, 0, 1);
    RawInput raw;
    raw.set_key(Key::K, true);
    replay->res<InputSystem>().feed_raw(raw);
    replay->run_frames(1);
    SettingsStore &settings = replay->res<SettingsStore>();
    (void)map.save(settings);
    std::printf("input_demo: jump slot 1 is now \"%s\"; settings input.p0.jump.1 = \"%s\"\n",
                binding_to_string(map.bindings(c.jump)[1]).c_str(), settings.get_string("input.p0.jump.1").c_str());
    std::filesystem::remove(path);
    return replayed == hash ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    const LaunchOptions options = parse_launch_options(argc, argv);
    if (options.headless) return headless(argc, argv);
    auto app = make_app(argc, argv, true);
    const int code = app->run();
    const Hero h = hero_of(*app);
    std::printf("input_demo: hero at (%.0f, %.0f), %d jumps, %d gifts\n", h.x, h.y, h.jumps, h.gifts);
    return code;
}
