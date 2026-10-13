#pragma once
// Window, clock and lifecycle behind the App. PlatformPlugin installs the raylib one; tests and
// --headless use NullPlatform.
#include <cstdint>
#include <lam/math/rect.h>
#include <lam/math/vec2.h>
namespace lam {
// OS lifecycle notifications, delivered as the App event LifecycleChanged.
enum class LifecycleEvent : std::uint8_t { Background, Foreground, FocusLost, FocusGained };
struct WindowInfo {
    IVec2 framebuffer;                                          // real pixels
    IVec2 window;                                               // points
    float dpi_scale = 1;                                        // framebuffer pixels per point
    bool focused = true;                                        // the window has keyboard focus
    bool fullscreen = false;                                    // borderless fullscreen is on
    IRect safe_area;                                            // framebuffer pixels
};
// Owned by the App (App::set_platform). Main thread only.
class Platform {
public:
    virtual ~Platform() = default;
    // The user asked to close the window; run() quits.
    virtual bool should_close() = 0;
    // Monotonic seconds from an arbitrary origin.
    virtual double now_seconds() = 0;
    // FrameBegin -10000: poll events, start drawing.
    virtual void begin_frame() = 0;
    // FrameEnd +10000: swap and poll. Runs every frame, even with --draw-every.
    virtual void end_frame() = 0;
    virtual WindowInfo window() const = 0;
    virtual void set_fullscreen(bool on) = 0;                   // borderless
    virtual bool has_gpu() const = 0;
};
// Headless: the clock advances exactly 1/tick_rate per begin_frame(). Never closes, no GPU.
class NullPlatform final : public Platform {
public:
    // framebuffer is also reported as the window size and safe area, at dpi 1.
    explicit NullPlatform(int tick_rate = 60, IVec2 framebuffer = {640, 360});
    bool should_close() override;
    double now_seconds() override;
    void begin_frame() override;
    void end_frame() override;
    WindowInfo window() const override;
    void set_fullscreen(bool on) override;                      // only records the flag
    bool has_gpu() const override;
private:
    int tick_rate_ = 60;
    IVec2 framebuffer_;
    std::uint64_t frames_ = 0;
    bool fullscreen_ = false;
};
// App events, emitted by the platform at FrameBegin -10000.
struct LifecycleChanged { LifecycleEvent event; };
struct WindowResized { IVec2 framebuffer; };
}
