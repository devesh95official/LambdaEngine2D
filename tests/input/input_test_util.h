#pragma once
// lam_input test helpers: StepApp, RawInput builders and a probe that records what ticks saw.
#include <memory>
#include <vector>

#include <lam/core/app.h>
#include <lam/core/platform.h>
#include <lam/input/actions.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/input/input_plugin.h>
#include <lam/input/raw_input.h>
#include <lam/test/support.h>

namespace lam::test {

// A platform whose clock only moves when the test says so.
class StepPlatform final : public Platform {
public:
    double t = 0;
    bool should_close() override { return false; }
    double now_seconds() override { return t; }
    void begin_frame() override {}
    void end_frame() override {}
    WindowInfo window() const override { return WindowInfo{{640, 360}, {640, 360}, 1, true, false, {0, 0, 640, 360}}; }
    void set_fullscreen(bool) override {}
    bool has_gpu() const override { return false; }
};

// Every tick of every world: the InputFrame of player 0..3 as the Input stage saw it.
struct Probe {
    std::vector<InputFrame> frames;                             // player 0, one per tick
    std::vector<InputFrame> p1;                                 // player 1
    std::uint64_t ticks = 0;
};

inline void probe_system(Ctx &ctx) {
    Probe &p = ctx.res<Probe>();
    p.frames.push_back(input_frame(ctx, 0));
    p.p1.push_back(input_frame(ctx, 1));
    ++p.ticks;
}

// A windowed-mode App (not --headless) on StepPlatform: frame(n) runs exactly n ticks (0..4).
// tick_rate 64 keeps the clock arithmetic exact.
class StepApp {
public:
    explicit StepApp(AppConfig config = make_config()) {
        static const char *const kArgv[] = {"lam_input_tests"};
        app_ = std::make_unique<App>(std::move(config), 1, kArgv);
        auto platform = std::make_unique<StepPlatform>();
        platform_ = platform.get();
        app_->set_platform(std::move(platform));
        app_->add_plugin<InputPlugin>();
        app_->insert_resource<Probe>();
        app_->add_system(Stage::Input, &probe_system, "test.probe");
        app_->run_frames(1);                                    // clock origin; runs 0 ticks
    }
    static AppConfig make_config() {
        AppConfig c;
        c.tick_rate = 64;
        c.settings_file = "";
        c.max_ticks_per_frame = 4;
        return c;
    }
    App &app() { return *app_; }
    InputSystem &input() { return app_->res<InputSystem>(); }
    ActionMap &map() { return app_->res<ActionMap>(); }
    Probe &probe() { return app_->res<Probe>(); }
    const FrameInput &frame_input() { return app_->res<FrameInput>(); }
    // One frame that runs exactly `ticks` ticks (0..max_ticks_per_frame).
    void frame(int ticks) {
        platform_->t += static_cast<double>(ticks) / 64.0;
        app_->run_frames(1);
    }
    // One frame with `raw` fed first (as the backend would at FrameBegin -5000).
    void frame(int ticks, const RawInput &raw) {
        input().feed_raw(raw);
        frame(ticks);
    }

private:
    std::unique_ptr<App> app_;
    StepPlatform *platform_ = nullptr;
};

inline RawInput keys_down(std::initializer_list<Key> keys) {
    RawInput r;
    for (Key k : keys)
        r.set_key(k, true);
    return r;
}

inline RawInput with_pad(RawInput r, int pad, std::initializer_list<PadButton> buttons,
                         std::initializer_list<std::pair<PadAxis, float>> axes = {}) {
    RawInput::Pad &p = r.pads[static_cast<std::size_t>(pad)];
    p.connected = true;
    for (PadButton b : buttons)
        p.buttons |= 1u << static_cast<unsigned>(b);
    for (auto [a, v] : axes)
        p.axes[static_cast<std::size_t>(a)] = v;
    return r;
}

} // namespace lam::test
