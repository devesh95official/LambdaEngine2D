// raylib Platform. Owns the window and the audio device, so other backends can rely on both
// between PlatformPlugin::build and shutdown (it's added first, so it shuts down last).
//
// One swap and one poll per frame. begin_frame (FrameBegin -10000) calls BeginDrawing, samples the
// window and emits WindowResized/LifecycleChanged. end_frame (FrameEnd +10000) calls EndDrawing,
// which flushes the batch, swaps and polls input; there's no SetTargetFPS, so it never sleeps.
// The render backend draws in between (FrameEnd -500) and never calls Begin/EndDrawing itself.
#include <lam/core/raylib_platform.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include <raylib.h>

#include <lam/core/debug.h>
#include <lam/core/events.h>
#include <lam/core/log.h>
#include <lam/core/platform.h>
#include <lam/core/settings.h>

#include "log_redirect.h"

namespace lam {
namespace {

// Fullscreen setting (bool). Written by F11 and the settings scene, applied by platform_hotkeys.
constexpr std::string_view kFullscreenKey = "display.fullscreen";
// A handful of platform events per frame at most.
constexpr std::size_t kPlatformEventCapacity = 16;

class RaylibPlatform final : public Platform {
public:
    // Check ready() afterwards. If the window didn't open, the platform acts as closed.
    RaylibPlatform(App &app, const AppConfig &config, const LaunchOptions &options) : app_(app) {
        unsigned int flags = FLAG_WINDOW_HIGHDPI;
        if (config.vsync && !options.no_vsync) flags |= FLAG_VSYNC_HINT;
        if (config.resizable) flags |= FLAG_WINDOW_RESIZABLE;
        if (options.hidden) flags |= FLAG_WINDOW_HIDDEN;
        SetConfigFlags(flags);
        InitWindow(config.window_size.x, config.window_size.y, config.title.c_str());
        ready_ = IsWindowReady();
        if (!ready_) return;
        SetExitKey(KEY_NULL); // Esc belongs to the game
        if (config.audio) {
            InitAudioDevice();
            audio_ = IsAudioDeviceReady();
            if (!audio_) LAM_LOG_WARN("platform: no audio device; sound is muted");
        }
        sample();
        minimized_ = IsWindowMinimized();
        last_seconds_ = GetTime();
    }

    ~RaylibPlatform() override { close(); }
    RaylibPlatform(const RaylibPlatform &) = delete;
    RaylibPlatform &operator=(const RaylibPlatform &) = delete;

    // True while the window is open.
    bool ready() const { return ready_; }

    // Closes the audio device, then the window. Idempotent. Every GPU and audio resource must be
    // released before this (backend plugin shutdowns run first).
    void close() {
        if (audio_) {
            CloseAudioDevice();
            audio_ = false;
        }
        if (ready_) {
            CloseWindow();
            ready_ = false;
        }
    }

    bool should_close() override { return !ready_ || WindowShouldClose(); }

    double now_seconds() override {
        if (ready_) last_seconds_ = GetTime();
        return last_seconds_;
    }

    void begin_frame() override {
        if (!ready_) return;
        BeginDrawing();
        poll_window_events();
    }

    void end_frame() override {
        if (!ready_) return;
        EndDrawing();
    }

    WindowInfo window() const override { return info_; }

    void set_fullscreen(bool on) override {
        if (!ready_ || on == is_fullscreen()) return;
        ToggleBorderlessWindowed();
        sample();
    }

    bool has_gpu() const override { return ready_; }

private:
    static bool is_fullscreen() {
        return IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) || IsWindowFullscreen();
    }

    // Refreshes info_. A minimised window may report a zero size; the last real size is kept.
    void sample() {
        const IVec2 framebuffer{GetRenderWidth(), GetRenderHeight()};
        if (framebuffer.x > 0 && framebuffer.y > 0) {
            info_.framebuffer = framebuffer;
            info_.window = IVec2{GetScreenWidth(), GetScreenHeight()};
            info_.safe_area = IRect{0, 0, framebuffer.x, framebuffer.y}; // desktop: whole buffer
        }
        info_.dpi_scale = GetWindowScaleDPI().x;
        info_.focused = IsWindowFocused();
        info_.fullscreen = is_fullscreen();
    }

    template <class T>
    void emit(const T &event) {
        if (Events<T> *channel = app_.events().try_get<T>()) channel->emit(event);
    }

    // Emits size and lifecycle changes seen since the last frame. Losing focus comes before going
    // to the background, and coming to the foreground before regaining focus.
    void poll_window_events() {
        const WindowInfo before = info_;
        const bool was_minimized = minimized_;
        sample();
        minimized_ = IsWindowMinimized();
        if (before.focused && !info_.focused) emit(LifecycleChanged{LifecycleEvent::FocusLost});
        if (!was_minimized && minimized_) emit(LifecycleChanged{LifecycleEvent::Background});
        if (was_minimized && !minimized_) emit(LifecycleChanged{LifecycleEvent::Foreground});
        if (!before.focused && info_.focused) emit(LifecycleChanged{LifecycleEvent::FocusGained});
        if (info_.framebuffer != before.framebuffer) emit(WindowResized{info_.framebuffer});
    }

    App &app_;          // owns this platform
    WindowInfo info_{}; // sampled at begin_frame
    double last_seconds_ = 0;
    bool ready_ = false;
    bool audio_ = false;
    bool minimized_ = false;
};

// App resource: plugin options and the last settings revision seen.
struct PlatformHotkeys {
    PlatformConfig config;
    bool hidden = false; // --hidden: never go fullscreen
    std::uint32_t settings_revision = 0;
};

// Picks up fullscreen changes made elsewhere (the settings scene) without depending on lam_ui.
void sync_fullscreen_setting(Platform &platform, SettingsStore &settings, PlatformHotkeys &state) {
    if (settings.revision() == state.settings_revision) return;
    state.settings_revision = settings.revision();
    if (state.hidden) return;
    const bool wanted = settings.get_bool(kFullscreenKey);
    if (wanted != platform.window().fullscreen) platform.set_fullscreen(wanted);
}

// FrameBegin -9800: F3 overlay and F4 colliders (LAM_DEV), F11 fullscreen (saved to settings).
void platform_hotkeys(Ctx &ctx) {
    PlatformHotkeys &state = ctx.res<PlatformHotkeys>();
#if LAM_DEV
    if (state.config.dev_hotkeys) {
        if (DebugOverlay *overlay = ctx.try_res<DebugOverlay>()) {
            if (IsKeyPressed(KEY_F3)) overlay->visible = !overlay->visible;
            if (IsKeyPressed(KEY_F4)) overlay->show_colliders = !overlay->show_colliders;
        }
    }
#endif
    Platform &platform = ctx.app().platform();
    SettingsStore *settings = ctx.try_res<SettingsStore>();
    if (state.config.f11_fullscreen && !state.hidden && IsKeyPressed(KEY_F11)) {
        const bool on = !platform.window().fullscreen;
        platform.set_fullscreen(on);
        if (settings) {
            settings->set_bool(kFullscreenKey, on);
            state.settings_revision = settings->revision();
        }
    }
    if (settings) sync_fullscreen_setting(platform, *settings, state);
}

template <class T>
void add_app_event(App &app) {
    if (app.events().try_get<T>() == nullptr)
        app.add_event<T>(EventScope::App, kPlatformEventCapacity);
}

} // namespace

PlatformPlugin::PlatformPlugin(PlatformConfig c) : config_(c) {}

void PlatformPlugin::build(App &app) {
    add_app_event<LifecycleChanged>(app);
    add_app_event<WindowResized>(app);
    if (app.options().headless) return; // NullPlatform stays installed

    detail::install_raylib_log_redirect();
    auto platform = std::make_unique<RaylibPlatform>(app, app.config(), app.options());
    if (!platform->ready()) {
        LAM_LOG_ERROR("platform: cannot open a window ({}x{}); quitting",
                      app.config().window_size.x, app.config().window_size.y);
        app.quit(1);
        return; // has_gpu() stays false
    }
    app.set_platform(std::move(platform));

    const bool hidden = app.options().hidden;
    PlatformHotkeys &state =
        app.insert_resource<PlatformHotkeys>(PlatformHotkeys{config_, hidden, 0});
    if (SettingsStore *settings = app.try_res<SettingsStore>()) {
        if (!settings->defined(kFullscreenKey)) settings->define_bool(kFullscreenKey, false);
        // Hidden windows (GPU tests) ignore the stored fullscreen setting.
        if (!hidden && settings->get_bool(kFullscreenKey)) {
            app.platform().set_fullscreen(true);
        }
        state.settings_revision = settings->revision();
    }
    app.add_system(SystemDesc{.stage = Stage::FrameBegin,
                              .fn = platform_hotkeys,
                              .name = "platform_hotkeys",
                              .order = -9800,
                              .scope = SystemScope::Once});
}

void PlatformPlugin::shutdown(App &app) {
    // Every other plugin has shut down by now, so their GPU and audio resources are already gone.
    if (auto *platform = dynamic_cast<RaylibPlatform *>(&app.platform())) platform->close();
}

} // namespace lam
