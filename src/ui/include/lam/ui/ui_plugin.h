#pragma once
// UiPlugin: a UiTree per world, UiNav and ToastQueue, theme and dialogue loaders, and the UI
// systems. Other modules' resources are optional, so it also runs in bare tests.
#include <string_view>
#include <lam/core/app.h>
namespace lam {
class UiPlugin final : public Plugin {
public:
    std::string_view name() const override { return "lam.ui"; }
    void build(App &app) override;
    void init_world(App &app, World &world) override;
    // Registers the loaders and default UiNav actions if Assets or Input was added after this.
    void finish(App &app) override;
private:
    bool loaders_ = false;                                      // theme/dialogue loaders registered
};
}
