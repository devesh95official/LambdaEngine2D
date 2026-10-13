#pragma once
// raylib window and audio device (target lam_platform). Esc doesn't close the window; the close
// button or App::quit ends the run.
#include <string_view>
#include <lam/core/app.h>
namespace lam {
struct PlatformConfig {
    bool f11_fullscreen = true;
    bool dev_hotkeys = true;                                    // F3 overlay, F4 colliders (LAM_DEV)
};
class PlatformPlugin final : public Plugin {
public:
    explicit PlatformPlugin(PlatformConfig c = {});
    std::string_view name() const override { return "lam.platform"; }
    void build(App &app) override;
    void shutdown(App &app) override;                           // added first, so it shuts down last
private:
    PlatformConfig config_;
};
}
