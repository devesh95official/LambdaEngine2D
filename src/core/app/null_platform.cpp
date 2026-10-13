// NullPlatform: the headless Platform used by tests, --headless and as the App default.
#include <lam/core/platform.h>

#include <algorithm>

namespace lam {

NullPlatform::NullPlatform(int tick_rate, IVec2 framebuffer)
    : tick_rate_(std::max(1, tick_rate)), framebuffer_(framebuffer) {}

bool NullPlatform::should_close() {
    return false;
}

double NullPlatform::now_seconds() {
    return static_cast<double>(frames_) / static_cast<double>(tick_rate_);
}

void NullPlatform::begin_frame() {
    ++frames_;
}

void NullPlatform::end_frame() {}

WindowInfo NullPlatform::window() const {
    WindowInfo w;
    w.framebuffer = framebuffer_;
    w.window = framebuffer_;
    w.dpi_scale = 1;
    w.focused = true;
    w.fullscreen = fullscreen_;
    w.safe_area = IRect{0, 0, framebuffer_.x, framebuffer_.y};
    return w;
}

void NullPlatform::set_fullscreen(bool on) {
    fullscreen_ = on;
}

bool NullPlatform::has_gpu() const {
    return false;
}

} // namespace lam
